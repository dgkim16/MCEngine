#include "MCEngine.h"
#include "DescHeapManager.h"
#include "FrameGraph/FrameGraph.h"
#include "FrameGraph/FrameGraphPasses.h"
#include "FrameGraph/LambdaPass.h"

// blackboard templates
namespace {
	struct ViewportColorData { FgResourceHandle handle; };
}

void MCEngine::InitializeRenderPass() {

	mPasses.push_back(std::make_unique<OpaquePass>(*this));
	mPasses.push_back(std::make_unique<OpaqueInstancedPass>(*this));
	mPasses.push_back(std::make_unique<LambdaPass>("GrassCullDispatch",
		[this](RenderPassBuilder& b) {
			auto* gc = mSceneManager.GetActive()->GetModule<MCGrassCullingModule>();
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
			auto* gc = mSceneManager.GetActive()->GetModule<MCGrassCullingModule>();
			if (!gc || !gc->useGpuCulling) return;
			auto* cmdList = ctx.cmdList;
			cmdList->CopyBufferRegion(
				gc->mGrassCounterBuffer.mResource.Get(), 0,
				gc->mGrassCounterResetBuffer.mResource.Get(), 0,
				sizeof(UINT));
			mBarrierManager.TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mBarrierManager.TransitionState(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mBarrierManager.FlushBarriers(cmdList);

			GrassCullCB cullCB = {};
			{
				// When frozen, use the snapshot captured at freeze time; otherwise use the live VP.
				XMMATRIX vpT = IsCameraFrozen()
					? XMLoadFloat4x4(&GetFrozenViewProjT())
					: XMLoadFloat4x4(&mMainPassCB.gViewProj);
				XMVECTOR rawPlanes[6] = {
					vpT.r[3] + vpT.r[0],   // left
					vpT.r[3] - vpT.r[0],   // right
					vpT.r[3] + vpT.r[1],   // bottom
					vpT.r[3] - vpT.r[1],   // top
					vpT.r[2],               // near  (DX NDC z >= 0)
					vpT.r[3] - vpT.r[2],   // far
				};
				for (int i = 0; i < 6; i++)
					XMStoreFloat4(&cullCB.FrustumPlanes[i], XMPlaneNormalize(rawPlanes[i]));
			}
			cullCB.EyePosW = mMainPassCB.gEyePosW;
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

			mBarrierManager.InsertUAVBarrier(gc->mGrassVisibleBuffer);
			mBarrierManager.InsertUAVBarrier(gc->mGrassCounterBuffer);
			mBarrierManager.TransitionState(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			mBarrierManager.TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
			mBarrierManager.TransitionState(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
			mBarrierManager.FlushBarriers(cmdList);

			mCommandList->CopyBufferRegion(
				gc->mGrassIndirectArgsBuffer.mResource.Get(),
				offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
				gc->mGrassCounterBuffer.mResource.Get(), 0,
				sizeof(UINT));

			mBarrierManager.TransitionState(gc->mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
			mBarrierManager.TransitionState(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			mBarrierManager.FlushBarriers(GetCmdList());
		}
	));
	mPasses.push_back(std::make_unique<LambdaPass>("GrassInstancedCulling",
		[this](RenderPassBuilder& b) { // SetupFn
			b.Write(mSceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
			b.Write(mSceneDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			auto* gc = mSceneManager.GetActive()->GetModule<MCGrassCullingModule>();
			if (gc && gc->useGpuCulling) {
				b.Read(gc->mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				b.Read(gc->mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
			}
		},
		[this](FgRenderContext& ctx) { // ExecuteFn
			auto* cmdList = ctx.cmdList;
			auto scene = Scenes().GetActive();
			if (!(scene->layers[(int)RenderLayer::GrassInstanced].size() > 0)) return;
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
					mCommandList->IASetVertexBuffers(0, 1, &vbv);
					mCommandList->IASetIndexBuffer(&ibv);
					mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);
					// mCommandList->SetGraphicsRootConstantBufferView(0, 0);

					auto objectCB = mCurrFrameResource->ObjectCB->Resource();
					UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerObjectCB));
					D3D12_GPU_VIRTUAL_ADDRESS handle = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
					mCommandList->SetGraphicsRootConstantBufferView(0, handle);

					mCommandList->SetGraphicsRootShaderResourceView(3,
						gc->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
					mCommandList->ExecuteIndirect(mGrassCommandSignature.Get(), 1,
						gc->mGrassIndirectArgsBuffer.mResource.Get(), 0, nullptr, 0);
				}
			}
			else { //! CPU culling path
				DrawInstanceRenderItems(mCommandList.Get(), scene->layers[(int)RenderLayer::GrassInstanced], true, "Grass Instanced Pass (CPU)");
			}
		}
	));

	mPasses.push_back(std::make_unique<AlphaTestedPass>(*this));
	mPasses.push_back(std::make_unique<AlphaTestedTreeSpritesPass>(*this));
	mPasses.push_back(std::make_unique<MirrorsPass>(*this));
	mPasses.push_back(std::make_unique<ReflectedPass>(*this));
	mPasses.push_back(std::make_unique<TransparentPass>(*this));
	mPasses.push_back(std::make_unique<ShadowPass>(*this));
	mPasses.push_back(std::make_unique<OpaqueTessellated>(*this));
	mPasses.push_back(std::make_unique<LambdaPass>("DebugLines",
		[this](RenderPassBuilder& b) {
			b.Write(mSceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
			mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
			mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			vbv.BufferLocation = mDebugLineVB[mCurrFrameResourceIndex]->GetGPUVirtualAddress();
			vbv.StrideInBytes = sizeof(XMFLOAT3);
			vbv.SizeInBytes = totalVerts * sizeof(XMFLOAT3);
			mCommandList->IASetVertexBuffers(0, 1, &vbv);

			if (mShowBoundingBoxes && mDebugBBoxVertCount > 0) {
				float yellow[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
				mCommandList->SetGraphicsRoot32BitConstants(0, 4, yellow, 0);
				mCommandList->DrawInstanced(mDebugBBoxVertCount, 1, mDebugBBoxVertStart, 0);
			}
			if (mShowFrustum && mDebugFrustVertCount > 0) {
				float cyan[4] = { 0.0f, 1.0f, 0.5f, 1.0f };
				mCommandList->SetGraphicsRoot32BitConstants(0, 4, cyan, 0);
				mCommandList->DrawInstanced(mDebugFrustVertCount, 1, mDebugFrustVertStart, 0);
			}
		}
	));

	mPasses.push_back(std::make_unique<LambdaPass>("SceneColorToViewportCopy/Resolve",
		[this](RenderPassBuilder& b) {
			auto& vpc = b.AddBlackboard<ViewportColorData>();
			FgResourceDesc desc{};
			desc.kind = FgResourceDesc::Kind::Texture2D;
			desc.format = mSceneFormat;
			desc.width = static_cast<uint32_t>(mSceneViewWidth);
			desc.height = static_cast<uint32_t>(mSceneViewHeight);
			desc.arraySize = 1;
			desc.mipLevels = 1;
			// ForceAlpha reads as SRV; no UAV flag needed. CopyResource / ResolveSubresource
			// don't require UAV on the destination.
			desc.flags = D3D12_RESOURCE_FLAG_NONE;
			desc.debugName = "ViewportColor";   // string-literal lifetime per D1 deviation #8
			vpc.handle = b.Create(desc);

			if (mScene4xMsaaState) {
				b.Read(mSceneColor, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
				b.Write(vpc.handle, D3D12_RESOURCE_STATE_RESOLVE_DEST);
			}
			else {
				b.Read(mSceneColor, D3D12_RESOURCE_STATE_COPY_SOURCE);
				b.Write(vpc.handle, D3D12_RESOURCE_STATE_COPY_DEST);
			}
		},
		[this](FgRenderContext& ctx) {
			auto* cmdList = ctx.cmdList;
			const auto& vpc = ctx.GetBlackboard<ViewportColorData>();
			ID3D12Resource* vpcRes = ctx.GetResource(vpc.handle);
			if (mScene4xMsaaState) {
				cmdList->ResolveSubresource(
					vpcRes, 0,
					mSceneColor.mResource.Get(), 0,
					mSceneFormat);
			}
			else {
				cmdList->CopyResource(
					vpcRes,
					mSceneColor.mResource.Get());
			}
		}
	));

	mPasses.push_back(std::make_unique<LambdaPass>("ForceAlpha",
		[this](RenderPassBuilder& b) {
			const auto& vpc = b.GetBlackboard<ViewportColorData>();
			b.Read(vpc.handle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			b.Write(mViewportNoAlpha, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		},
		[this](FgRenderContext& ctx) {
			auto* cmdList = ctx.cmdList;
			const auto& vpc = ctx.GetBlackboard<ViewportColorData>();
			CSB_default cb{};
			cb.InputIndex = ctx.GetSRV(vpc.handle);
			cb.OutputIndex = (INT)mViewportNoAlpha.UAVs[0].offset;
			// Write/bind this frame's own CB slot — the buffer holds gNumFrameResources
			// copies so the GPU reads the slot this frame wrote, not a later frame's overwrite.
			mForceAlphaUploadBuffer->CopyData(mCurrFrameResourceIndex, cb);
			cmdList->SetPipelineState(mPSOs["forceAlphaOne"].Get());
			cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
			cmdList->SetComputeRootConstantBufferView(0,
				mForceAlphaUploadBuffer->Resource()->GetGPUVirtualAddress()
				+ mCurrFrameResourceIndex * d3dUtil::CalcConstantBufferByteSize(sizeof(CSB_default)));
			const UINT gx = static_cast<UINT>(std::ceil(mSceneViewWidth / 16.0f));
			const UINT gy = static_cast<UINT>(std::ceil(mSceneViewHeight / 16.0f));
			cmdList->Dispatch(gx, gy, 1);
		}
	));

	auto blurred1Handle = std::make_shared<FgResourceHandle>();
	mPasses.push_back(std::make_unique<LambdaPass>("Blur",
		[this, blurred1Handle](RenderPassBuilder& b) {
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
				*blurred1Handle = b.Create(desc);
				b.Read(mViewportNoAlpha, D3D12_RESOURCE_STATE_COPY_SOURCE);
				b.Write(*blurred1Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				b.Write(mBlurred0, D3D12_RESOURCE_STATE_COPY_DEST);
			}
		},
		[this, blurred1Handle](FgRenderContext& ctx) {
			if (blurValues.enabled || (mSobelType == SobelType::Gaussain && mIsSobel)) {
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
					blur.OutputIndex = (INT)ctx.GetUAV(*blurred1Handle);
					mBlurUploadBuffer.get()->CopyData(mCurrFrameResourceIndex * 2 + 0, blur);
					blur.InputIndex = (INT)ctx.GetSRV(*blurred1Handle);
					blur.OutputIndex = (INT)mBlurred0.UAVs[0].offset;
					mBlurUploadBuffer.get()->CopyData(mCurrFrameResourceIndex * 2 + 1, blur);
				}
				
				auto* cmdList = ctx.cmdList;
				MCResource& blurred1MC = ctx.GetMCResource(*blurred1Handle);
				mBarrierManager.TransitionState(mViewportNoAlpha, D3D12_RESOURCE_STATE_COPY_SOURCE);
				mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_COPY_DEST);
				mBarrierManager.TransitionState(blurred1MC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				mBarrierManager.FlushBarriers(mCommandList.Get());
				mCommandList->CopyResource(mBlurred0.mResource.Get(),
					mViewportNoAlpha.mResource.Get());

				// ID3D12Resource* blurred1Res = ctx.GetResource(*blurred1Handle);


				const auto blurCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(CSB_blur));
				// Per-frame base: this frame owns CB slots [frame*2, frame*2+1] = (H, V).
				const auto blurCBBase = mBlurUploadBuffer->Resource()->GetGPUVirtualAddress()
					+ (mCurrFrameResourceIndex * 2) * blurCBByteSize;
				const auto blurCBaddrH = blurCBBase;
				const auto blurCBaddrV = blurCBBase + blurCBByteSize;
				const UINT blurGroupsX = static_cast<UINT>(std::ceil(mSceneViewWidth / 256.0f));
				const UINT blurGroupsY = static_cast<UINT>(std::ceil(mSceneViewHeight / 256.0f));

				for (int i = 0; i < blurValues.blurIter; i++) {
					mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					mBarrierManager.TransitionState(blurred1MC, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					mBarrierManager.FlushBarriers(mCommandList.Get());
					cmdList->SetPipelineState(mPSOs["horzBlur"].Get());
					cmdList->SetComputeRootConstantBufferView(0, blurCBaddrH);
					cmdList->Dispatch(blurGroupsX, static_cast<UINT>(mSceneViewHeight), 1);
					// mBarrierManager.InsertUAVBarrier(blurred1MC);
					// mBarrierManager.FlushBarriers(mCommandList.Get());

					mBarrierManager.TransitionState(mBlurred0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					mBarrierManager.TransitionState(blurred1MC, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					mBarrierManager.FlushBarriers(mCommandList.Get());
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
				b.Read(mViewportNoAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				switch (mSobelType) {
				case SobelType::Default:
					b.Read(mViewportNoAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
					b.Read(mViewportNoAlpha, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
						csbs.InputIndex = (INT)mViewportNoAlpha.SRVs[0].offset;
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
						csbs.Width = (INT)mViewportNoAlpha.SRVs[0].offset; // viewport with no alpha srv
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


	mFrameGraph->MarkRoot(mSceneColor);
	
	// Resources consumed outside the graph during intermediate migration states.
	// Each is a temporary root until the consumer also moves into the graph.
	// mFrameGraph->MarkRoot(mViewportColor);     // consumed by hand-batched ForceAlpha (deletes in Step 1.2)

	// ImGui - these either don't exist or become transients w/o ImGui
	mFrameGraph->MarkRoot(mViewportNoAlpha);   // consumed by ImGui::Image at MC_imgui.cpp:290; never migrates
	mFrameGraph->MarkRoot(mBlurred0);          // consumed by ImGui::Image at MC_imgui.cpp:289; never migrates
	mFrameGraph->MarkRoot(mSobelOutput);       // consumed by ImGui::Image at MC_imgui.cpp:288; never migrates
	mFrameGraph->MarkRoot(mSceneDepth);        // consumed by ImGui::Image when MSAA off; consumed by DepthViz hand-batched code.
	mFrameGraph->MarkRoot(mDepthDebugColor);   // consumed by ImGui::Image at MC_imgui.cpp:367; never migrates

	for (auto& p : mPasses) {
		p->mTimerId = mGpuTimer.Register(p->TypeName());
		mFrameGraph->RegisterPass(p.get());
	}

	
}

/*


mPasses.push_back(std::make_unique<LambdaPass>("SceneColorToViewportCopy",
	[this](RenderPassBuilder& b) {

	},
	[this](FgRenderContext& ctx) {

	}
));


*/