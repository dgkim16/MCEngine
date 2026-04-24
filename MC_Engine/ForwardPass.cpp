#include "MCEngine.h"
#include "DescHeapManager.h"
#include "Scene_grass.h"
#include <pix3.h>

void MCEngine::ForwardPass(const GameTimer& gt) {

	// Flush any dirty tier staging writes into the shader-visible combined heap
	// before any draw references bindless indices.
	DescHeapManager::Get().CommitToShaderVisible();

	ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvSrvUavHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	mCommandList->SetGraphicsRootSignature(mRootSignatures[0].Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	auto matCB = mCurrFrameResource->MaterialCB->Resource();
	
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress()); // setup per pass cbv
	mCommandList->SetGraphicsRootShaderResourceView(2, matCB->GetGPUVirtualAddress());

	// Always set a graphics PSO before the first draw — GrassCullDispatch may have
	// left a compute PSO active, and the Opaque layer has no other PSO set.
	mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["opaque_MSAA"].Get() : mPSOs["opaque"].Get());

	// PrintRenderItemInLayers();
	if (mRitemLayer[(int)RenderLayer::Opaque].size() > 0) {
		if (mIsWireframe)
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["opaque_wireframe_MSAA"].Get() : mPSOs["opaque_wireframe"].Get());
		else
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["opaque_MSAA"].Get() : mPSOs["opaque"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque],"opaque Pass");
	}

	if (mRitemLayer[(int)RenderLayer::OpaqueInstanced].size() > 0) {
		if(mIsWireframe)
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["opaque_instanced_tess_wireframe_MSAA"].Get() : mPSOs["opaque_instanced_tess_wireframe"].Get());
		else
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["opaque_instanced_tess_MSAA"].Get() : mPSOs["opaque_instanced_tess"].Get());
		DrawInstanceRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::OpaqueInstanced],false, "opaque Instanced Pass");
	}

	if (mRitemLayer[(int)RenderLayer::GrassInstanced].size() > 0) {
		if (mIsWireframe)
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["grass_instanced_wireframe_MSAA"].Get() : mPSOs["grass_instanced_wireframe"].Get());
		else
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["grass_instanced_MSAA"].Get() : mPSOs["grass_instanced"].Get());

		if (mGrassScene && mGrassScene->useGpuCulling) {
			//! GPU culling path: instances already compacted into mGrassVisibleBuffer by GrassCullDispatch
			PIXBeginEvent(mCommandList.Get(), PIX_COLOR(0, 200, 100), "Grass Instanced Pass (GPU)");
			for (auto& ri : mRitemLayer[(int)RenderLayer::GrassInstanced]) {
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
					mGrassScene->mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
				mCommandList->ExecuteIndirect(mGrassCommandSignature.Get(), 1,
					mGrassScene->mGrassIndirectArgsBuffer.mResource.Get(), 0, nullptr, 0);
			}
			PIXEndEvent(mCommandList.Get());
		} else {
			//! CPU culling path: InstanceCB was populated by UpdateInstanceData
			DrawInstanceRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::GrassInstanced],true, "Grass Instanced Pass (CPU)");
		}
	}
	

	if (mRitemLayer[(int)RenderLayer::AlphaTested].size() > 0) {
		mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["alphaTested_MSAA"].Get() : mPSOs["alphaTested"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTested], "Alpha Tested Pass");
	}

	if (mRitemLayer[(int)RenderLayer::AlphaTestedTreeSprites].size() > 0) {
		if (mIsWireframe)
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["treeSprites_wireframe_MSAA"].Get() : mPSOs["treeSprites_wireframe"].Get());
		else
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["treeSprites_MSAA"].Get() : mPSOs["treeSprites"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTestedTreeSprites], "treeSprites Pass");
	}

	// Mark the visible mirror pixels in the stencil buffer with the value 1
	mCommandList->OMSetStencilRef(1);
	mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["markStencilMirrors_MSAA"].Get() : mPSOs["markStencilMirrors"].Get());
	if (mRitemLayer[(int)RenderLayer::Mirrors].size() > 0) {
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Mirrors], "Mirrors Pass");
	}
	// UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PerPassCB));
	// mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress() + 1 * passCBByteSize);
	// currently they are NOT being reflected, so pass values of view and projection matrices are the same
	if (mRitemLayer[(int)RenderLayer::Reflected].size() > 0) {
		// Draw the reflection into the mirror only (only for pixels where the stencil buffer is 1).
		// Note that we must supply a different per-pass constant buffer--one with the lights reflected.			
		if (mIsWireframe)
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["drawStencilReflections_wireframe_MSAA"].Get() : mPSOs["drawStencilReflections_wireframe"].Get());
		else
			mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["drawStencilReflections_MSAA"].Get() : mPSOs["drawStencilReflections"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Reflected], "Reflected Pass");
	}
	// Restore main pass constants and stencil ref.
	// mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
	mCommandList->OMSetStencilRef(0);

	if (mRitemLayer[(int)RenderLayer::Transparent].size() > 0) {
		mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["transparent_MSAA"].Get() : mPSOs["transparent"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Transparent], "Transparent Pass");
	}
	if (mRitemLayer[(int)RenderLayer::Shadow].size() > 0) {
		mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["shadow_MSAA"].Get() : mPSOs["shadow"].Get());
		DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Shadow],"Shadow Pass");
	}

	// ---- Debug visualization (bounding boxes + frustum) ----
	if (mShowBoundingBoxes || mShowFrustum) {
		BuildDebugLineGeometry(); // this 'build' runs every frame if enabled. quite heavy, but debug so trivial
		UINT totalVerts = mDebugBBoxVertCount + mDebugFrustVertCount;
		if (totalVerts > 0) {
			mCommandList->SetGraphicsRootSignature(mDebugLineRootSig.Get());
			mCommandList->SetPipelineState(mScene4xMsaaState
				? mPSOs["debug_line_MSAA"].Get()
				: mPSOs["debug_line"].Get());

			mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
			mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

			D3D12_VERTEX_BUFFER_VIEW vbv = {};
			vbv.BufferLocation = mDebugLineVB[mCurrFrameResourceIndex]->GetGPUVirtualAddress();
			vbv.StrideInBytes  = sizeof(XMFLOAT3);
			vbv.SizeInBytes    = totalVerts * sizeof(XMFLOAT3);
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

			// Restore main root signature + descriptor heap so TessellationExample
			// (and any other subsequent pass) can use slot 0 as a CBV again.
			// ID3D12DescriptorHeap* mainHeaps[] = { mCbvHeap.Get() };
			// mCommandList->SetDescriptorHeaps(_countof(mainHeaps), mainHeaps);
			mCommandList->SetGraphicsRootSignature(mRootSignatures[0].Get());
			mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());
			mCommandList->SetGraphicsRootShaderResourceView(2, matCB->GetGPUVirtualAddress());
		}
	}

}

void MCEngine::TessellationExample(const GameTimer& gt) {
	if (!mTessellatedRitem) return;
	if (mIsWireframe)
		mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["tessellation_wireframe_MSAA"].Get() : mPSOs["tessellation_wireframe"].Get());
	else
		mCommandList->SetPipelineState(mScene4xMsaaState ? mPSOs["tessellation_MSAA"].Get() : mPSOs["tessellation"].Get());
	std::set<RenderItem*> setTessellRI = { mTessellatedRitem };
	DrawRenderItems(mCommandList.Get(), setTessellRI, "Tessellation Pass");
}
