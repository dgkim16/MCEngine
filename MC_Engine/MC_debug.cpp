#include "MCEngine.h"
struct LayerDebugInfo
{
	RenderLayer layer;
	const char* name;
};

static size_t counts[] = { 0, 0, 0, 0, 0, 0, 0 };

const LayerDebugInfo layers[] =
{
	{ RenderLayer::Opaque,      "Opaque" },
	{ RenderLayer::AlphaTested, "AlphaTested" },
	{ RenderLayer::Transparent, "Transparent" },
	{ RenderLayer::Mirrors,     "Mirrors" },
	{ RenderLayer::Reflected,   "Reflected" },
	{ RenderLayer::Shadow,      "Shadow" },
	{ RenderLayer::AlphaTestedTreeSprites,      "AlphaTestedTreeSprites" },
	{ RenderLayer::OpaqueTessellated,      "OpaqueTessellated" },
	{ RenderLayer::OpaqueInstanced,      "OpaqueInstanced" },
	{ RenderLayer::AlphaTestedInstanced,      "AlphaTestedInstanced" },
};

void MCEngine::PrintRenderItemInLayers() {
	bool chng = false;
	for (int i = 0; i < 6; ++i) {
		size_t curr = mRitemLayer[(int)layers[i].layer].size();
		if (counts[i] != curr) {
			std::cout << layers[i].name << " : " << counts[i] << " >> "<<curr<< std::endl;
			counts[i] = curr;
			chng = true;
		}
	}
	if (chng) std::cout << "-------------------------" << std::endl;
}


// ============================================================
//  Debug Visualization
// ============================================================

void MCEngine::BuildDebugLineResources()
{
	const UINT bufSize = kMaxDebugLineVerts * sizeof(XMFLOAT3);
	auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(bufSize);

	mDebugLineVB.resize(gNumFrameResources);
	mDebugLineVBMapped.resize(gNumFrameResources, nullptr);
	for (int i = 0; i < gNumFrameResources; ++i) {
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&mDebugLineVB[i])));
		ThrowIfFailed(mDebugLineVB[i]->Map(0, nullptr,
			reinterpret_cast<void**>(&mDebugLineVBMapped[i])));
	}
}

// Returns total vertex count written. Populates the per-range offsets/counts.
UINT MCEngine::BuildDebugLineGeometry()
{
	XMFLOAT3* verts = mDebugLineVBMapped[mCurrFrameResourceIndex];
	UINT      vc = 0;

	// Writes 12 edges (24 verts) for a set of 8 world-space box corners.
	auto addBoxEdges = [&](XMFLOAT3 corners[8]) {
		static const int kEdges[12][2] = {
			{0,1},{1,2},{2,3},{3,0},
			{4,5},{5,6},{6,7},{7,4},
			{0,4},{1,5},{2,6},{3,7}
		};
		for (auto& e : kEdges) {
			verts[vc++] = corners[e[0]];
			verts[vc++] = corners[e[1]];
		}
		};

	// ---- Bounding boxes (yellow) ----
	mDebugBBoxVertStart = vc;
	if (mShowBoundingBoxes) {
		for (auto& ri : mAllRitems) {
			if (vc + 24 > kMaxDebugLineVerts) break;
			// Skip instanced layers — per-instance transforms aren't in RenderItem::World
			int layer = ri->Mat ? ri->Mat->renderLevel : -1;
			if (layer == (int)RenderLayer::OpaqueInstanced ||
				layer == (int)RenderLayer::GrassInstanced)
				continue;
			// Skip items with a degenerate bounds
			const auto& ext = ri->Bounds.Extents;
			if (ext.x == 0.0f && ext.y == 0.0f && ext.z == 0.0f) continue;

			XMFLOAT3 corners[8];
			ri->Bounds.GetCorners(corners);
			XMMATRIX world = XMLoadFloat4x4(&ri->World);
			for (int i = 0; i < 8; ++i) {
				XMVECTOR v = XMVector3Transform(XMLoadFloat3(&corners[i]), world);
				XMStoreFloat3(&corners[i], v);
			}
			addBoxEdges(corners);
		}
	}
	mDebugBBoxVertCount = vc - mDebugBBoxVertStart;

	// ---- Frustum (cyan-green) ----
	mDebugFrustVertStart = vc;
	if (mShowFrustum && vc + 24 <= kMaxDebugLineVerts) {
		DirectX::BoundingFrustum worldFrustum;
		if (mFreezeCamera) {
			worldFrustum = mFrozenWorldFrustum;
		}
		else {
			XMMATRIX view = mMainCamera->GetView();
			XMVECTOR det = XMMatrixDeterminant(view);
			XMMATRIX invV = XMMatrixInverse(&det, view);
			mMainCamera->GetFrustrum().Transform(worldFrustum, invV);
		}
		XMFLOAT3 corners[8];
		worldFrustum.GetCorners(corners);
		addBoxEdges(corners);
	}
	mDebugFrustVertCount = vc - mDebugFrustVertStart;

	return vc;
}

