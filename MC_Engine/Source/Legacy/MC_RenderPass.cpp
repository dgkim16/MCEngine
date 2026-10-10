#include "Legacy/MCEngine.h"
#include "Renderer/D3D12/DescHeapManager.h"
#include "Renderer/D3D12/FrameGraph/FrameGraph.h"
#include "Legacy/FrameGraphPasses.h"
#include "Renderer/D3D12/FrameGraph/LambdaPass.h"
#include "Legacy/MCLight.h"

// blackboard templates
namespace {
	// Published by Blur's Setup (b.AddBlackboard), read by its Execute
	// (ctx.GetBlackboard). Blackboard is cleared every FrameGraph::Begin, so the
	// handle never outlives the frame that created the transient.
	struct Blurred1Data { FgResourceHandle handle; };
}

void MCEngine::InitializeRenderPass() {
	std::string shadowPassName = "ShadowPass_";

	// ================= OPAQUE SHADOW PASS
	for (int i = 0; i < MCLightManager::Get().LightCount(); ++i) {
		std::string w = shadowPassName + std::to_string(i);
		mPasses.push_back(std::make_unique<LambdaPass>(std::move(w),
			[this, i](RenderPassBuilder& b) {
				b.Write(*MCLightManager::Get().GetLight(i).mShadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			},
			[this, i](FgRenderContext& ctx) {
				auto& f_light = MCLightManager::Get().GetLight(i);
				if (!f_light.mData.mCastShadow) return;
				auto* cmdList = ctx.cmdList;
				auto& fShadowMap = f_light.mShadowMap;
				const float resolution = static_cast<float>(f_light.GetShadowMapResolution());
				D3D12_VIEWPORT vp = { 0.0f, 0.0f, resolution, resolution, 0.0f, 1.0f };
				D3D12_RECT sc = { 0.0f, 0.0f, static_cast<long>(resolution),static_cast<long>(resolution) };
				cmdList->RSSetViewports(1, &vp);
				cmdList->RSSetScissorRects(1, &sc);
				ID3D12DescriptorHeap* heaps[] = { mCbvSrvUavHeap.Get() };
				cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
				cmdList->ClearDepthStencilView(fShadowMap->DSVs[0].hCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				cmdList->OMSetRenderTargets(0, nullptr, false, &fShadowMap->DSVs[0].hCpu);

				cmdList->SetGraphicsRootSignature(mRootSignatures[0].Get());
				cmdList->SetPipelineState(mPSOs["shadowPass"].Get());
				auto passCB = mCurrFrameResource->PassCB->Resource();
				UINT alignmentSize = mCurrFrameResource->PassCB->ElementByteSize();
				cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress() + (1 + i) * alignmentSize);
				cmdList->SetGraphicsRootShaderResourceView(2, mCurrFrameResource->MaterialCB->Resource()->GetGPUVirtualAddress()); // Vertex Shader reads from this
				cmdList->SetGraphicsRootShaderResourceView(4, mCurrFrameResource->ObjectCB->Resource()->GetGPUVirtualAddress()); // Vertex Shader reads from this
				cmdList->SetGraphicsRootShaderResourceView(5, mCurrFrameResource->LightCB->Resource()->GetGPUVirtualAddress()); // Vertex Shader reads from this
				DrawLayer(ctx.cmdList, RenderLayer::Opaque, true, "opaque shadow Pass");
			}
		));
	}
	mPasses.push_back(std::make_unique<LambdaPass>("SetUp-ForwardPass",
		[this](RenderPassBuilder& b) {
			b.Write(ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
			b.Write(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			for (int i = 0; i < MCLightManager::Get().LightCount(); ++i) {
				if (!MCLightManager::Get().GetLight(i).mData.mCastShadow) continue;
				auto& fShadowMap = MCLightManager::Get().GetLight(i).mShadowMap;
				b.Read(*fShadowMap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		},
		[this](FgRenderContext& ctx) {
			auto* cmdList = ctx.cmdList;
			cmdList->RSSetViewports(1, &mSceneViewport);
			cmdList->RSSetScissorRects(1, &mSceneScissorRect);

			auto sceneRtv = ForwardPassWriteTarget().RTVs[0].hCpu;
			auto sceneDsv = mSceneDepth.DSVs[0].hCpu;
			cmdList->ClearRenderTargetView(sceneRtv, (float*)&mFogColor, 0, nullptr);
			cmdList->ClearDepthStencilView(sceneDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			cmdList->OMSetRenderTargets(1, &sceneRtv, true, &sceneDsv);

			ID3D12DescriptorHeap* heaps[] = { mCbvSrvUavHeap.Get() };
			cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
			cmdList->SetGraphicsRootSignature(mRootSignatures[0].Get());
			auto passCB = mCurrFrameResource->PassCB->Resource();
			auto matCB = mCurrFrameResource->MaterialCB->Resource();
			auto objCB = mCurrFrameResource->ObjectCB->Resource();
			auto lightCB = mCurrFrameResource->LightCB->Resource();
			cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
			cmdList->SetGraphicsRootShaderResourceView(2, matCB->GetGPUVirtualAddress());
			cmdList->SetGraphicsRootShaderResourceView(4, objCB->GetGPUVirtualAddress());
			cmdList->SetGraphicsRootShaderResourceView(5, lightCB->GetGPUVirtualAddress());
		}
	));
	mPasses.push_back(std::make_unique<OpaquePass>(*this));
	mPasses.push_back(std::make_unique<OpaqueInstancedPass>(*this));
	mPasses.push_back(std::make_unique<LambdaPass>("GrassCullDispatch",
		[this](RenderPassBuilder& b) {
			auto* scene = mSceneManager.GetActive();	// null after a failed load
			auto* gc = scene ? scene->GetModule<MCGrassCullingModule>() : nullptr;
			if (!gc || !gc->useGpuCulling) return;
			// End-state declarations. The intra-pass UAV-to-COPY_SOURCE transition on
			// mGrassCounterBuffer happens inside Execute (we BarrierManager::InsertUAVBarrier
			// + FlushBarriers manually because it's a within-pass barrier; the graph's
			// state model only sees start/end states per pass).
			b.Read(gc->mGrassFullInstanceBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			b.Write(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			b.Write(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
			b.Write(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
		},
		[this](FgRenderContext& ctx) { // moved MCGrassCullingModule::OnDraw into here
			auto* scene = mSceneManager.GetActive();	// null after a failed load
			auto* gc = scene ? scene->GetModule<MCGrassCullingModule>() : nullptr;
			if (!gc || !gc->useGpuCulling) return;
			auto* cmdList = ctx.cmdList;
			cmdList->CopyBufferRegion(
				gc->mGrassCounterBuffer.mResource.Get(), 0,
				gc->mGrassCounterResetBuffer.mResource.Get(), 0,
				sizeof(UINT));
			ctx.barriers->TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ctx.barriers->TransitionState(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ctx.barriers->FlushBarriers(cmdList);

			GrassCullCB cullCB = {};
			{
				// When frozen, use the snapshot captured at freeze time; otherwise use the live VP.
				DirectX::XMMATRIX vpT = IsCameraFrozen()
					? XMLoadFloat4x4(&GetFrozenViewProjT())
					: XMLoadFloat4x4(&GetMainPassCB().gViewProj);
				using DirectX::operator+;	// XMVECTOR is __m128, so ADL does not find DirectX's operators
				using DirectX::operator-;
				DirectX::XMVECTOR rawPlanes[6] = {
					vpT.r[3] + vpT.r[0],   // left
					vpT.r[3] - vpT.r[0],   // right
					vpT.r[3] + vpT.r[1],   // bottom
					vpT.r[3] - vpT.r[1],   // top
					vpT.r[2],               // near  (DX NDC z >= 0)
					vpT.r[3] - vpT.r[2],   // far
				};
				for (int i = 0; i < 6; i++)
					XMStoreFloat4(&cullCB.FrustumPlanes[i], DirectX::XMPlaneNormalize(rawPlanes[i]));
			}
			cullCB.EyePosW = GetMainPassCB().gEyePosW;
			cullCB.DrawDistance = 500.0f;
			cullCB.InstanceCount = gc->mTotalGrassInstances;
			cullCB.GrassMaterialIndex = gc->mGrassMaterial->MatCBIndex; // always current, even after reassignment
			cullCB.SphereRadius = std::get<MCMeshSource::GrassPatchParams>(
				Meshes().GetSource(gc->mBladeMeshHandle)->params).height;
			gc->mGrassCullCB[mCurrFrameResourceIndex]->CopyData(0, cullCB);

			cmdList->SetComputeRootSignature(mGrassCullRootSignature.Get());
			cmdList->SetPipelineState(mPSOs["grass_cull_cs"].Get());
			cmdList->SetComputeRootConstantBufferView(0, gc->mGrassCullCB[mCurrFrameResourceIndex]->Resource()->GetGPUVirtualAddress());
			cmdList->SetComputeRootShaderResourceView(1, gc->mGrassFullInstanceBuffer.mResource->GetGPUVirtualAddress());
			cmdList->SetComputeRootUnorderedAccessView(2, gc->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
			cmdList->SetComputeRootUnorderedAccessView(3, gc->mGrassCounterBuffer.mResource->GetGPUVirtualAddress());
			const UINT groups = (gc->mTotalGrassInstances + 63) / 64;
			cmdList->Dispatch(groups, 1, 1);

			ctx.barriers->InsertUAVBarrier(gc->mGrassVisibleBuffer);
			ctx.barriers->InsertUAVBarrier(gc->mGrassCounterBuffer);
			ctx.barriers->TransitionState(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ctx.barriers->TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
			ctx.barriers->TransitionState(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
			ctx.barriers->FlushBarriers(cmdList);

			cmdList->CopyBufferRegion(
				gc->mGrassIndirectArgsBuffer.mResource.Get(),
				offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
				gc->mGrassCounterBuffer.mResource.Get(), 0,
				sizeof(UINT));

			ctx.barriers->TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
			ctx.barriers->TransitionState(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			ctx.barriers->FlushBarriers(cmdList);
		}
	));
	// IF shadows for grass, must do it HERE
	/*
	for (int i = 0; i < MCLightManager::Get().LightCount(); ++i) {
		mPasses.push_back(std::make_unique<LambdaPass>("GrassInstancedShadowPass",
			[this,i](RenderPassBuilder& b) { // SetupFn
				b.Write(*MCLightManager::Get().GetLight(i).mShadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				auto* scene = mSceneManager.GetActive();	// null after a failed load
				auto* gc = scene ? scene->GetModule<MCGrassCullingModule>() : nullptr;
				if (gc && gc->useGpuCulling) {
					b.Read(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					b.Read(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
				}
			},
			[this,i](FgRenderContext& ctx) { // ExecuteFn
				auto scene = Scenes().GetActive();
				if (!scene || !(scene->layers[(int)RenderLayer::GrassInstanced].size() > 0)) return;

				auto* cmdList = ctx.cmdList;
				auto& f_light = MCLightManager::Get().GetLight(i);
				if (!f_light.mData.mCastShadow) return;
				auto& fShadowMap = f_light.mShadowMap;
				const float resolution = static_cast<float>(f_light.GetShadowMapResolution());
				D3D12_VIEWPORT vp = { 0, 0, resolution, resolution, 0.0f, 1.0f };
				D3D12_RECT sc = { 0, 0, resolution,resolution };
				cmdList->RSSetViewports(1, &vp);
				cmdList->RSSetScissorRects(1, &sc);
				ID3D12DescriptorHeap* heaps[] = { mCbvSrvUavHeap.Get() };
				cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
				cmdList->ClearDepthStencilView(fShadowMap->DSVs[0].hCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				cmdList->OMSetRenderTargets(0, nullptr, false, &fShadowMap->DSVs[0].hCpu);

				cmdList->SetGraphicsRootSignature(mRootSignatures[0].Get());
				cmdList->SetPipelineState(mPSOs["shadowPass"].Get());
				auto passCB = mCurrFrameResource->PassCB->Resource();
				UINT alignmentSize = mCurrFrameResource->PassCB->ElementByteSize();
				cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress() + (1 + i) * alignmentSize);
				cmdList->SetGraphicsRootShaderResourceView(5, mCurrFrameResource->LightCB->Resource()->GetGPUVirtualAddress()); // Vertex Shader reads from this


				
				auto* gc = scene->GetModule<MCGrassCullingModule>();
				if (gc && gc->useGpuCulling) { //! GPU culling path
					for (auto& ri : scene->layers[(int)RenderLayer::GrassInstanced]) {
						auto vbv = ri->Geo->VertexBufferView();
						auto ibv = ri->Geo->IndexBufferView();
						cmdList->IASetVertexBuffers(0, 1, &vbv);
						cmdList->IASetIndexBuffer(&ibv);
						cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
						// mCommandList->SetGraphicsRootConstantBufferView(0, 0);

						cmdList->SetGraphicsRoot32BitConstant(0, ri->ObjCBIndex, 0);

						cmdList->SetGraphicsRootShaderResourceView(3,
							gc->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
						cmdList->ExecuteIndirect(mGrassCommandSignature.Get(), 1,
							gc->mGrassIndirectArgsBuffer.mResource.Get(), 0, nullptr, 0);
					}
				}
				else { //! CPU culling path
					DrawInstanceRenderItems(cmdList, scene->layers[(int)RenderLayer::GrassInstanced], true, "Grass Instanced Pass (CPU)");
				}
			}
		));
	}
	*/
	mPasses.push_back(std::make_unique<LambdaPass>("GrassInstancedDraw",
		[this](RenderPassBuilder& b) { // SetupFn
			b.Write(ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
			b.Write(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			auto* scene = mSceneManager.GetActive();	// null after a failed load
			auto* gc = scene ? scene->GetModule<MCGrassCullingModule>() : nullptr;
			if (gc && gc->useGpuCulling) {
				b.Read(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				b.Read(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			}
		},
		[this](FgRenderContext& ctx) { // ExecuteFn
			auto* cmdList = ctx.cmdList;
			auto scene = Scenes().GetActive();
			if (!scene || !(scene->layers[(int)RenderLayer::GrassInstanced].size() > 0)) return;
			auto* gc = scene->GetModule<MCGrassCullingModule>();
			const char* psoName =
				mIsWireframe
				? (mScene4xMsaaState ? "grass_instanced_wireframe_MSAA" : "grass_instanced_wireframe")
				: (mScene4xMsaaState ? "grass_instanced_MSAA" : "grass_instanced");
			cmdList->SetPipelineState(GetPSO(psoName));
			if (gc && gc->useGpuCulling) { //! GPU culling path
				for (auto& ri : scene->layers[(int)RenderLayer::GrassInstanced]) {
					auto vbv = ri->Geo->VertexBufferView();
					auto ibv = ri->Geo->IndexBufferView();
					cmdList->IASetVertexBuffers(0, 1, &vbv);
					cmdList->IASetIndexBuffer(&ibv);
					cmdList->IASetPrimitiveTopology(ri->PrimitiveType);
					// mCommandList->SetGraphicsRootConstantBufferView(0, 0);

					cmdList->SetGraphicsRoot32BitConstant(0, ri->ObjCBIndex, 0);

					cmdList->SetGraphicsRootShaderResourceView(3,
						gc->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
					cmdList->ExecuteIndirect(mGrassCommandSignature.Get(), 1,
						gc->mGrassIndirectArgsBuffer.mResource.Get(), 0, nullptr, 0);
				}
			}
			else { //! CPU culling path
				DrawInstanceRenderItems(cmdList, scene->layers[(int)RenderLayer::GrassInstanced], true, "Grass Instanced Pass (CPU)");
			}
		}
	));

	mPasses.push_back(std::make_unique<AlphaTestedPass>(*this));
	mPasses.push_back(std::make_unique<AlphaTestedTreeSpritesPass>(*this));
	mPasses.push_back(std::make_unique<MirrorsPass>(*this));
	mPasses.push_back(std::make_unique<ReflectedPass>(*this));
	mPasses.push_back(std::make_unique<TransparentPass>(*this));
	mPasses.push_back(std::make_unique<OpaqueTessellated>(*this));
	mPasses.push_back(std::make_unique<LambdaPass>("DebugLines",
		[this](RenderPassBuilder& b) {
			b.Write(ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
			b.Write(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		},
		[this](FgRenderContext& ctx) {
			auto* cmdList = ctx.cmdList;
			if (!mShowBoundingBoxes && !mShowFrustum) return;
			BuildDebugLineGeometry();
			const UINT totalVerts = mDebugBBoxVertCount + mDebugFrustVertCount;
			if (totalVerts == 0) return;

			cmdList->SetGraphicsRootSignature(mDebugLineRootSig.Get());
			const char* psoName = mScene4xMsaaState ? "debug_line_MSAA" : "debug_line";
			cmdList->SetPipelineState(GetPSO(psoName));
			auto passCB = mCurrFrameResource->PassCB->Resource();
			cmdList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			vbv.BufferLocation = mDebugLineVB[mCurrFrameResourceIndex]->GetGPUVirtualAddress();
			vbv.StrideInBytes = sizeof(DirectX::XMFLOAT3);
			vbv.SizeInBytes = totalVerts * sizeof(DirectX::XMFLOAT3);
			cmdList->IASetVertexBuffers(0, 1, &vbv);

			if (mShowBoundingBoxes && mDebugBBoxVertCount > 0) {
				float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
				cmdList->SetGraphicsRoot32BitConstants(0, 4, yellow, 0);
				cmdList->DrawInstanced(mDebugBBoxVertCount, 1, mDebugBBoxVertStart, 0);
			}
			if (mShowFrustum && mDebugFrustVertCount > 0) {
				float cyan[4] = { 0.0f, 1.0f, 0.5f, 1.0f };
				cmdList->SetGraphicsRoot32BitConstants(0, 4, cyan, 0);
				cmdList->DrawInstanced(mDebugFrustVertCount, 1, mDebugFrustVertStart, 0);
			}
		}
	));

	mPasses.push_back(std::make_unique<LambdaPass>("MSAA resolve",
		[this](RenderPassBuilder& b) {
			if (!mScene4xMsaaState)
				return;
			b.Read(mSceneColorMSAA, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
			b.Write(mSceneColor, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			
		},
		[this](FgRenderContext& ctx) {
			if (!mScene4xMsaaState)
				return;
			auto* cmdList = ctx.cmdList;
			cmdList->ResolveSubresource(
				mSceneColor.mResource.Get(), 0,
				mSceneColorMSAA.mResource.Get(), 0,
				mSceneFormat);
			
		}
	));
	mPasses.push_back(std::make_unique<LambdaPass>("Blur",
		[this](RenderPassBuilder& b) {
			if (blurValues.enabled || (mSobelType == SobelType::Gaussain && mIsSobel)) {
				static FgResourceDesc desc{};
				desc.kind = FgResourceDesc::Kind::Texture2D;
				desc.format = mSceneFormat;
				desc.width = static_cast<uint32_t>(mSceneViewWidth);
				desc.height = static_cast<uint32_t>(mSceneViewHeight);
				desc.arraySize = 1;
				desc.mipLevels = 1;
				desc.flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				desc.debugName = "Blurred1";   // string-literal lifetime per D1 deviation #8
				auto& bd = b.AddBlackboard<Blurred1Data>();
				bd.handle = b.Create(desc);
				b.Read(mSceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE); // mViewportNoAlpha
				b.Write(bd.handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				b.Write(mBlurred0, D3D12_RESOURCE_STATE_COPY_DEST);
			}
		},
		[this](FgRenderContext& ctx) {
			if (blurValues.enabled || (mSobelType == SobelType::Gaussain && mIsSobel)) {
				const auto& bd = ctx.GetBlackboard<Blurred1Data>();
				// UpdateBlurCB
				{
					static CSB_blur blur = {}; // heap access, may not be ideal, but works. optimize later.
					if (blurDirty) {
						auto w = CalcGaussWeights(blurValues.sigma);
						blur.BlurRadius = (int)w.size() / 2;
						ZeroMemory(blur.WeightVec, sizeof(blur.WeightVec));
						CopyMemory(blur.WeightVec, w.data(), w.size() * sizeof(float));
						blurDirty = false;
					}
					blur.InputIndex = (INT)mBlurred0.SRVs[0].offset;
					blur.OutputIndex = (INT)ctx.GetUAV(bd.handle);
					mBlurUploadBuffer.get()->CopyData(mCurrFrameResourceIndex * 2 + 0, blur);
					blur.InputIndex = (INT)ctx.GetSRV(bd.handle);
					blur.OutputIndex = (INT)mBlurred0.UAVs[0].offset;
					mBlurUploadBuffer.get()->CopyData(mCurrFrameResourceIndex * 2 + 1, blur);
				}

				auto* cmdList = ctx.cmdList;
				MCResource& blurred1MC = ctx.GetMCResource(bd.handle);
				ctx.barriers->TransitionState(mSceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE); // mViewportNoAlpha
				ctx.barriers->TransitionState(mBlurred0, D3D12_RESOURCE_STATE_COPY_DEST);
				ctx.barriers->TransitionState(blurred1MC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				ctx.barriers->FlushBarriers(cmdList);
				cmdList->CopyResource(mBlurred0.mResource.Get(),
					mSceneColor.mResource.Get()); // mViewportNoAlpha

				// ID3D12Resource* blurred1Res = ctx.GetResource(bd.handle);


				const auto blurCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(CSB_blur));
				// Per-frame base: this frame owns CB slots [frame*2, frame*2+1] = (H, V).
				const auto blurCBBase = mBlurUploadBuffer->Resource()->GetGPUVirtualAddress()
					+ (mCurrFrameResourceIndex * 2) * blurCBByteSize;
				const auto blurCBaddrH = blurCBBase;
				const auto blurCBaddrV = blurCBBase + blurCBByteSize;
				const UINT blurGroupsX = static_cast<UINT>(std::ceil(mSceneViewWidth / 256.0f));
				const UINT blurGroupsY = static_cast<UINT>(std::ceil(mSceneViewHeight / 256.0f));

				cmdList->SetComputeRootSignature(mComputeRootSignature.Get());

				for (int i = 0; i < blurValues.blurIter; i++) {
					ctx.barriers->TransitionState(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					ctx.barriers->TransitionState(blurred1MC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					ctx.barriers->FlushBarriers(cmdList);
					cmdList->SetPipelineState(mPSOs["horzBlur"].Get());
					cmdList->SetComputeRootConstantBufferView(0, blurCBaddrH);
					cmdList->Dispatch(blurGroupsX, static_cast<UINT>(mSceneViewHeight), 1);
					// mBarrierManager.InsertUAVBarrier(blurred1MC);
					// mBarrierManager.FlushBarriers(mCommandList.Get());

					ctx.barriers->TransitionState(mBlurred0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					ctx.barriers->TransitionState(blurred1MC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					ctx.barriers->FlushBarriers(cmdList);
					cmdList->SetPipelineState(mPSOs["vertBlur"].Get());
					cmdList->SetComputeRootConstantBufferView(0, blurCBaddrV);
					cmdList->Dispatch(static_cast<UINT>(mSceneViewWidth), blurGroupsY, 1);
					// mBarrierManager.InsertUAVBarrier(mBlurred0);
					// mBarrierManager.FlushBarriers(mCommandList.Get());
				}
			}
		}
	));

	mPasses.push_back(std::make_unique<LambdaPass>("Sobel",
		[this](RenderPassBuilder& b) {
			if (mIsSobel) {
				b.Read(mSceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); // mViewportNoAlpha
				switch (mSobelType) {
				case SobelType::Default:
					b.Read(mSceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); // mViewportNoAlpha
					break;
				case SobelType::Depth:
					b.Read(mDepthDebugColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); 
					break;
				case SobelType::Gaussain:
				default:
					b.Read(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				}

				if (blurValues.enabled)
					b.Read(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				else
					b.Read(mSceneColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); // mViewportNoAlpha
				b.Write(mSobelOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
		},
		[this](FgRenderContext& ctx) {
			if (mIsSobel) {
				// UpdateSobelCB
				{
					CSB_default csbs;
					// INputIndex, OutputIndex, Widht, Height	
					// corresponds to...
					// gBlurIndex(to sobel), gOutputIndex, gSceneIndex (to mult by sobeled), PADDING (not used)
					switch (mSobelType) {
					case SobelType::Default:
						csbs.InputIndex = (INT)mSceneColor.SRVs[0].offset; // mViewportNoAlpha
						break;
					case SobelType::Depth:
						csbs.InputIndex = (INT)mDepthDebugColor.SRVs[0].offset;
						break;
					case SobelType::Gaussain:
					default:
						csbs.InputIndex = (INT)mBlurred0.SRVs[0].offset;
						break;
					}
					if (blurValues.enabled)
						csbs.Width = (INT)mBlurred0.SRVs[0].offset;
					else
						csbs.Width = (INT)mSceneColor.SRVs[0].offset; // viewport with no alpha srv // mViewportNoAlpha
					csbs.OutputIndex = (INT)mSobelOutput.UAVs[0].offset;

					mSobelUploadBuffer.get()->CopyData(mCurrFrameResourceIndex, csbs);
					mSobelUploadBuffer.get()->Resource()->SetName(L"mSobelUploadBuffer");
				}

				auto* cmdList = ctx.cmdList;
				cmdList->SetPipelineState(mPSOs["sobel"].Get());
				cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
				cmdList->SetComputeRootConstantBufferView(0,
					mSobelUploadBuffer->Resource()->GetGPUVirtualAddress()
					+ mCurrFrameResourceIndex * d3dUtil::CalcConstantBufferByteSize(sizeof(CSB_default)));
				const UINT gx = static_cast<UINT>(std::ceil(mSceneViewWidth / 8.0f));
				const UINT gy = static_cast<UINT>(std::ceil(mSceneViewHeight / 8.0f));
				cmdList->Dispatch(gx, gy, 1);
			}
		}
	));
	mPasses.push_back(std::make_unique<LambdaPass>("DepthViz",
		[this](RenderPassBuilder& b) {
			b.Read(mSceneDepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			b.Write(mDepthDebugColor, D3D12_RESOURCE_STATE_RENDER_TARGET);

		},
		[this](FgRenderContext& ctx) {
			auto* cmdList = ctx.cmdList;
			cmdList->SetPipelineState(mScene4xMsaaState ? mPSOs["depthDebug_MSAA"].Get() : mPSOs["depthDebug"].Get());
			auto depthDebugRtv = mDepthDebugColor.RTVs[0].hCpu;
			cmdList->OMSetRenderTargets(1, &depthDebugRtv, TRUE, nullptr);
			cmdList->ClearRenderTargetView(depthDebugRtv, DirectX::Colors::Black, 0, nullptr);
			cmdList->SetGraphicsRootSignature(mRootSignatures[1].Get());
			cmdList->SetGraphicsRootDescriptorTable(0, mSceneDepth.SRVs[0].hGpu); // raw scene depth SRV
			cmdList->SetGraphicsRootConstantBufferView(1, mCurrFrameResource->DepthCB.get()->Resource()->GetGPUVirtualAddress());

			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->DrawInstanced(3, 1, 0, 0);
		}
	));


	mFrameGraph->MarkRoot(mSceneColor);
	
	// Resources consumed outside the graph during intermediate migration states.
	// Each is a temporary root until the consumer also moves into the graph.
	// mFrameGraph->MarkRoot(mViewportColor);     // consumed by hand-batched ForceAlpha (deletes in Step 1.2)

	// ImGui - these either don't exist or become transients w/o ImGui
	// mFrameGraph->MarkRoot(mViewportNoAlpha);   // consumed by ImGui::Image at MC_imgui.cpp:290; never migrates // mViewportNoAlpha
	mFrameGraph->MarkRoot(mBlurred0);          // consumed by ImGui::Image at MC_imgui.cpp:289; never migrates
	mFrameGraph->MarkRoot(mSobelOutput);       // consumed by ImGui::Image at MC_imgui.cpp:288; never migrates
	mFrameGraph->MarkRoot(mSceneDepth);        // consumed by ImGui::Image when MSAA off; consumed by DepthViz hand-batched code.
	mFrameGraph->MarkRoot(mDepthDebugColor);   // consumed by ImGui::Image at MC_imgui.cpp:367; never migrates

	for (auto& p : mPasses) {
		mFrameGraph->RegisterPass(p.get());
	}

	
}
