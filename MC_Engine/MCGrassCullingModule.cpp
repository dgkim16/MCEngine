#include "MCGrassCullingModule.h"
#include "MCEngine.h"
#include "MCAssetIdentity.h"
#include "imgui.h"
#include "MCScene.h"
#include <DirectXMath.h>
#include <random>
#include <numeric>

void MCGrassCullingModule::OnLoad(MCEngine& engine, MCScene& scene) {
	mGrassRitem = scene.nameToRitem.at(grassRitemName);
	mPlaneRitem = scene.nameToRitem.at(planeRitemName);
	mBladeMeshHandle = mGrassRitem->meshHandle;
	PopulateGrassInstances(engine);          // builds GrassInstances + InstanceCells
	BuildGpuCullingBuffers(engine);
}

void MCGrassCullingModule::OnActivate(MCEngine& engine, MCScene& scene) {
	mGrassRitem = scene.nameToRitem.at(grassRitemName);
	mPlaneRitem = scene.nameToRitem.at(planeRitemName);
	CacheMaterialPointers(engine);           // mGrassMaterial/mPlaneMaterial via Get(handle)
	if (mGrassRitem->GrassInstances.empty()) PopulateGrassInstances(engine);
	// No engine-side registration — consumers query scene.GetModule<MCGrassCullingModule>().
}

void MCGrassCullingModule::OnDeactivate(MCEngine& engine, MCScene&) {
	// module's lifetime ends with scene.modules; nothing to unregister.
}

void MCGrassCullingModule::OnUpdate(MCEngine& engine, MCScene& scene, float dt) {

}

// entire code was moved into ExecuteFn of FrameGraph's lambda pass 'GrassCullDispatch'
void MCGrassCullingModule::OnDraw(MCEngine& engine, MCScene& scene) {
	return;
	// entire logic moved to RenderPass
	if (!useGpuCulling) return;
	auto* mCommandList = engine.GetCmdList();
	auto& mBarrierManager = engine.GetBarrierManager();
	auto& mMainPassCB = engine.GetMainPassCB();
	auto vp = mMainPassCB.gViewProj;

	// 1. Reset counter to 0 (counterReset is GENERIC_READ, counter is COPY_DEST)
	// note that actual counter is GPU-memory (default heap), so to update it
	// we need to do something like UpdateSubresource() with an upload heap CPU-memory
	// but that is a helper function that uses raw cpu-memory data (const void* wrapped in D3D12_SUBRESOURCE_DATA)
	// This is a lower level API that allows direct copying from one resource to another
	// see documentation for requirements of using CopyBufferRegion
	mCommandList->CopyBufferRegion(
		mGrassCounterBuffer.mResource.Get(), 0,
		mGrassCounterResetBuffer.mResource.Get(), 0,
		sizeof(UINT));

	// 2. Transition visible buffer and counter to UAV for CS writes
	mBarrierManager.TransitionState(mGrassVisibleBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	mBarrierManager.TransitionState(mGrassCounterBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	mBarrierManager.FlushBarriers(engine.GetCmdList());
	/*
	CD3DX12_RESOURCE_BARRIER toUAV[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassVisibleBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
	};
	mCommandList->ResourceBarrier(_countof(toUAV), toUAV);
	*/
	// 3. Update CullCB — extract world-space frustum planes on CPU, send to shader
	// mMainPassCB.gViewProj stores Transpose(M_vp), so its rows equal the columns of M_vp.
	// Gribb-Hartmann (row-vector DX convention): planes come from columns of M_vp.
	GrassCullCB cullCB = {};
	{
		// When frozen, use the snapshot captured at freeze time; otherwise use the live VP.
		XMMATRIX vpT = engine.IsCameraFrozen()
			? XMLoadFloat4x4(&engine.GetFrozenViewProjT())
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
	cullCB.InstanceCount = mTotalGrassInstances;
	cullCB.GrassMaterialIndex = mGrassMaterial->MatCBIndex; // always current, even after reassignment
	cullCB.SphereRadius = std::get<MCMeshSource::GrassPatchParams>(
	engine.Meshes().GetSource(mBladeMeshHandle)->params).height;
	// mGrassCullCB->CopyData(0, cullCB);
	mGrassCullCB[engine.GetCurrFrameResourceIndex()]->CopyData(0, cullCB);

	// 4. Dispatch culling CS (one thread per grass instance)
	mCommandList->SetComputeRootSignature(engine.GetGrassCullRootSignature());
	mCommandList->SetPipelineState(engine.GetPSO("grass_cull_cs"));
	// mCommandList->SetComputeRootConstantBufferView(0, mGrassCullCB->Resource()->GetGPUVirtualAddress());
	mCommandList->SetComputeRootConstantBufferView(0, mGrassCullCB[engine.GetCurrFrameResourceIndex()]->Resource()->GetGPUVirtualAddress());
	mCommandList->SetComputeRootShaderResourceView(1, mGrassFullInstanceBuffer.mResource->GetGPUVirtualAddress());
	mCommandList->SetComputeRootUnorderedAccessView(2, mGrassVisibleBuffer.mResource->GetGPUVirtualAddress());
	mCommandList->SetComputeRootUnorderedAccessView(3, mGrassCounterBuffer.mResource->GetGPUVirtualAddress());
	UINT groups = (mTotalGrassInstances + 63) / 64;
	mCommandList->Dispatch(groups, 1, 1);

	/*
	// 5. UAV barriers: ensure CS writes are visible before reads
	// this stalls subsequent commands until writes to UAVs are complete
	CD3DX12_RESOURCE_BARRIER uavBarriers[] = {
		CD3DX12_RESOURCE_BARRIER::UAV(mGrassVisibleBuffer.mResource.Get()),
		CD3DX12_RESOURCE_BARRIER::UAV(mGrassCounterBuffer.mResource.Get()),
	};
	mCommandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
	*/
	mBarrierManager.InsertUAVBarrier(mGrassVisibleBuffer);
	mBarrierManager.InsertUAVBarrier(mGrassCounterBuffer);
	// mBarrierManager.FlushBarriers(engine.GetCmdList());

	mBarrierManager.TransitionState(mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	mBarrierManager.TransitionState(mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
	mBarrierManager.TransitionState(mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	mBarrierManager.FlushBarriers(engine.GetCmdList());
	// 6. Transition: visible → SRV; counter → COPY_SOURCE; indirect args → COPY_DEST
	/*
	CD3DX12_RESOURCE_BARRIER postCS[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassVisibleBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassIndirectArgsBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
			D3D12_RESOURCE_STATE_COPY_DEST),
	};
	mCommandList->ResourceBarrier(_countof(postCS), postCS);
	*/

	// 7. Copy visible count into indirect args InstanceCount field
	mCommandList->CopyBufferRegion(
		mGrassIndirectArgsBuffer.mResource.Get(),
		offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
		mGrassCounterBuffer.mResource.Get(), 0,
		sizeof(UINT));

	/*
	// 8. Transition indirect args → INDIRECT_ARGUMENT; counter → COPY_DEST for next frame
	CD3DX12_RESOURCE_BARRIER postCopy[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassIndirectArgsBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
		CD3DX12_RESOURCE_BARRIER::Transition(mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_DEST),
	};
	mCommandList->ResourceBarrier(_countof(postCopy), postCopy);
	*/
	mBarrierManager.TransitionState(mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	mBarrierManager.TransitionState(mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	mBarrierManager.FlushBarriers(engine.GetCmdList());
}

void MCGrassCullingModule::OnImGui(MCEngine& engine, MCScene& scene) {
	static bool reloadOnChange = false;
	ImGui::Checkbox("ReloadOnChange", &reloadOnChange);
	ImGui::Checkbox("GPU Culling (Compute Shader)", &useGpuCulling);
	if (useGpuCulling) {
		ImGui::TextDisabled("Visible count: GPU-side only");
		ImGui::TextDisabled("Cell culling disabled in GPU mode");
	}
	else {
		ImGui::Text("Visible instance count : %d", mGrassRitem->InstanceCount);
		ImGui::Text("BoundingBox Test count : %d", mGrassRitem->bbInstTestCount);
	}
	ImGui::Separator();
	ImGui::Text("Cells: %d x %d = %d cells", numCellsX, numCellsZ,
		(int)mGrassRitem->InstanceCells.size());

	ImGui::Checkbox("Cell culling (voxel)", &mGrassRitem->useCellCulling);
	if (mGrassRitem->useCellCulling) {
		bool rebuildCells = false;
		rebuildCells |= ImGui::SliderInt("numCellsX", &numCellsX, 1, 20);
		rebuildCells |= ImGui::SliderInt("numCellsZ", &numCellsZ, 1, 20);
		rebuildCells &= reloadOnChange;
		if (rebuildCells) engine.RequestReload();
	}
	auto& bp = std::get<MCMeshSource::GrassPatchParams>(
		engine.Meshes().GetSource(mBladeMeshHandle)->params);
	ImGui::Text("%.2f, %.2f, %.2f", bp.width, bp.height, bp.sharpness);
	bool rebuildBlade = false;
	rebuildBlade |= ImGui::SliderFloat("blade sharpness", &bp.sharpness, 0.01f, 1.0f);
	rebuildBlade |= ImGui::SliderFloat("blade Width", &bp.width, 0.01f, 1.0f);
	rebuildBlade |= ImGui::SliderFloat("blade Height", &bp.height, 0.01f, 1.0f);
	ImGui::Separator();
	ImGui::Text("%d, %d", grassCountWidth, grassCountDepth);
	rebuildBlade |= ImGui::SliderInt("grassCountWidth", &grassCountWidth, 1, 1000);
	rebuildBlade |= ImGui::SliderInt("grassCountDepth", &grassCountDepth, 1, 1000);
	ImGui::Separator();
	ImGui::Text("%.2f, %.2f", grassCoverageWidth, grassCoverageDepth);
	rebuildBlade |= ImGui::SliderFloat("grassCoverageWidth", &grassCoverageWidth, 1.0f, 1000.0f);
	rebuildBlade |= ImGui::SliderFloat("grassCoverageDepth", &grassCoverageDepth, 1.0f, 1000.0f);
	ImGui::Separator();
	rebuildBlade &= reloadOnChange;
	if (rebuildBlade) engine.RequestReload();
	XMFLOAT4& colorGrass = mGrassMaterial->DiffuseAlbedo;
	bool fogSync = ImGui::Button("SyncFog");
	ImGui::Checkbox("Equal color", &equalColor);
	bool grassChng = ImGui::ColorPicker3("grass color", &colorGrass.x, 0);
	if (grassChng || fogSync) {
		if (fogSync) mGrassMaterial->DiffuseAlbedo = engine.GetFogColor();
		mGrassMaterial->NumFramesDirty = gNumFrameResources;
		mGrassRitem->NumFramesDirty = gNumFrameResources;
		XMVECTOR v = XMLoadFloat4(&colorGrass);
		XMVECTOR result = v * 0.3f;
		if (equalColor) {
			XMStoreFloat4(&mPlaneMaterial->DiffuseAlbedo, result);
			mPlaneMaterial->NumFramesDirty = gNumFrameResources;
		}
	}
	if (equalColor)
		return;
	XMFLOAT4& colorPlane = mPlaneMaterial->DiffuseAlbedo;
	if (ImGui::ColorPicker3("plane color", &colorPlane.x, 0))
		mPlaneMaterial->NumFramesDirty = gNumFrameResources;

}

void MCGrassCullingModule::CacheMaterialPointers(MCEngine& engine) {
	// Lifted verbatim from v1 Scene_grass::BuildMaterials (deprec/Scene_grass.cpp:120-127).
	// Materials are loaded once at engine init via mMaterialManager.LoadDirectory; by
	// the time OnActivate fires they are guaranteed registered. Throws if either
	// material is missing — that would indicate Assets/materials/ is incomplete.
	mGrassMaterial = engine.Materials().Get(HashAssetIdentity<AssetKind::Material>("grass"));
	mPlaneMaterial = engine.Materials().Get(HashAssetIdentity<AssetKind::Material>("grassPlane"));
	if (!mGrassMaterial)
		throw std::runtime_error("MCGrassCullingModule: material 'grass' not registered");
	if (!mPlaneMaterial)
		throw std::runtime_error("MCGrassCullingModule: material 'grassPlane' not registered");
}

void MCGrassCullingModule::PopulateGrassInstances(MCEngine& engine) {
	using namespace DirectX;
	// Deterministic regeneration of mGrassRitem->GrassInstances from public
	// scene params. Called from BuildRenderItems on initial Load and from
	// Activate() when GrassInstances is empty (post-JSON-reload). Same RNG
	// seed (12345) so regenerated data matches the GPU instance buffer
	// uploaded once at Load via BuildGpuCullingBuffers.
	if (!mGrassRitem) return;

	float w = grassCoverageWidth;
	float d = grassCoverageDepth;
	UINT grassCount = grassCountWidth * grassCountDepth;

	mGrassRitem->GrassInstances.clear();
	mGrassRitem->GrassInstances.resize(grassCount);
	OutputDebugString(L"started instancing render item\n");

	std::mt19937 rng(12345);
	std::uniform_real_distribution<float> jitterDist(-0.4f, 0.4f);
	std::uniform_real_distribution<float> rotDist(0.0f, XM_2PI);
	std::uniform_real_distribution<float> scaleDist(0.85f, 1.15f);

	float x = -0.5f * w;
	float z = -0.5f * d;

	float cellW = w / grassCountWidth;
	float cellD = d / grassCountDepth;

	for (int k = 0; k < grassCountDepth; k++)
	{
		for (int i = 0; i < grassCountWidth; i++) {
			int index = k * grassCountWidth + i;
			float px = x + (i + 0.5f) * cellW + jitterDist(rng) * cellW;
			float pz = z + (k + 0.5f) * cellD + jitterDist(rng) * cellD; // randomize for every grass, not per depth
			float rotY = rotDist(rng);
			float scale = scaleDist(rng);
			auto posfl3 = XMFLOAT3(px, 0.0f, pz);
			XMVECTOR position = XMLoadFloat3(&posfl3);
			XMStoreFloat3(&mGrassRitem->GrassInstances[index].grassPosition, position);
			mGrassRitem->GrassInstances[index].cosYaw = cos(rotY);
			mGrassRitem->GrassInstances[index].sinYaw = sin(rotY);
			mGrassRitem->GrassInstances[index].scale = scale;
		}
	}
	OutputDebugString(L"completed building render item!\n");

	// Sort GrassInstances by cell + build InstanceCells (CPU-culling input).
	BuildInstanceCells(engine);
}

void MCGrassCullingModule::BuildInstanceCells(MCEngine& engine) {
	float w = grassCoverageWidth;
	float d = grassCoverageDepth;
	float cellFootW = w / numCellsX;
	float cellFootD = d / numCellsZ;

	// Expand cell AABB slightly beyond footprint to account for blade radius + scale
	auto& bp = std::get<MCMeshSource::GrassPatchParams>(engine.Meshes().GetSource(mBladeMeshHandle)->params);
	float bladeHalfW = bp.width * 1.15f * 0.5f;
	float bladeHeight = bp.height * 1.15f;

	int totalInstances = (int)mGrassRitem->GrassInstances.size();

	// 1. Assign cell index to each instance based on world-space XZ translation
	//    World._41 = tx, World._43 = tz
	std::vector<int> cellIndex(totalInstances);
	for (int idx = 0; idx < totalInstances; idx++) {
		// float tx = mGrassRitem->GrassInstances[idx].World._41;
		// float tz = mGrassRitem->GrassInstances[idx].World._43;
		float tx = mGrassRitem->GrassInstances[idx].grassPosition.x;
		float tz = mGrassRitem->GrassInstances[idx].grassPosition.z;
		int cx = (int)((tx + 0.5f * w) / cellFootW);
		int cz = (int)((tz + 0.5f * d) / cellFootD);
		cx = std::clamp(cx, 0, numCellsX - 1);
		cz = std::clamp(cz, 0, numCellsZ - 1);
		cellIndex[idx] = cz * numCellsX + cx;
	}
	// 2. Sort Instances[] by cell index so members are contiguous
	std::vector<int> order(totalInstances);
	std::iota(order.begin(), order.end(), 0); // fill with [0,1,2,...,totalInstances-1]
	std::sort(order.begin(), order.end(),
		[&](int a, int b) { return cellIndex[a] < cellIndex[b]; });

	std::vector<GrassInstanceData> sorted(totalInstances);
	for (int i = 0; i < totalInstances; i++)
		sorted[i] = mGrassRitem->GrassInstances[order[i]]; // copy by value (expensive)
	mGrassRitem->GrassInstances = std::move(sorted);

	// Remap cellIndex array to match sorted order
	std::vector<int> sortedCellIdx(totalInstances);
	for (int i = 0; i < totalInstances; i++)
		sortedCellIdx[i] = cellIndex[order[i]];

	// 3. Build InstanceCell entries (run-length encode sortedCellIdx)
	mGrassRitem->InstanceCells.clear();
	int i = 0;
	while (i < totalInstances) {
		// use i that was incremented upto first blade 
		// of current cell from prev loop to get lienar cell index
		int ci = sortedCellIdx[i];
		int start = i;

		// increment i upto next cell (will be used on next loop)
		while (i < totalInstances && sortedCellIdx[i] == ci) i++;

		// reverse flattened cell coordinate from 1d back to to 2d 
		// ci -> (cx,cz)
		int cx = ci % numCellsX;
		int cz = ci / numCellsX;

		// compute bounds for BoundingBox (ymin fixed to 0.0f for now)
		float xmin = -0.5f * w + cx * cellFootW - bladeHalfW;
		float xmax = xmin + cellFootW + bladeHalfW * 2.0f;
		float zmin = -0.5f * d + cz * cellFootD - bladeHalfW;
		float zmax = zmin + cellFootD + bladeHalfW * 2.0f;
		float ymin = 0.0f;
		float ymax = bladeHeight;

		InstanceCell cell;
		cell.WorldBounds = DirectX::BoundingBox(
			DirectX::XMFLOAT3{ (xmin + xmax) * 0.5f, (ymin + ymax) * 0.5f, (zmin + zmax) * 0.5f },
			DirectX::XMFLOAT3{ (xmax - xmin) * 0.5f, (ymax - ymin) * 0.5f, (zmax - zmin) * 0.5f }
		);
		cell.StartIndex = start;
		cell.Count = i - start;
		mGrassRitem->InstanceCells.push_back(cell);
	}
}

void MCGrassCullingModule::BuildGpuCullingBuffers(MCEngine& engine) {
	mGrassFullInstanceBuffer.mResource.Reset();
	mGrassIndirectArgsBuffer.mResource.Reset();
	mGrassVisibleBuffer.mResource.Reset();
	mGrassCounterBuffer.mResource.Reset();
	mGrassCounterResetBuffer.mResource.Reset();

	mGrassCullCB = {};

	ID3D12Device* device = engine.GetDevice();
	ID3D12GraphicsCommandList* cmdList = engine.GetCmdList();

	mTotalGrassInstances = (UINT)mGrassRitem->GrassInstances.size();
	UINT64 instBufSize = (UINT64)mTotalGrassInstances * sizeof(GrassInstanceData);

	// 1. Full instance buffer (SRV, written once at load from CPU-side Instances[])
	// Matrices are transposed to match UpdateInstanceData()'s convention:
	// HLSL uses mul(v, M) with column-major storage, so C++ row-major matrices
	// must be transposed before upload. Without this, _41/_42/_43 in the cull
	// shader reads zeros instead of the translation components.
	mGrassFullInstanceBuffer.mResource = d3dUtil::CreateDefaultBuffer(
		device, cmdList,
		mGrassRitem->GrassInstances.data(), instBufSize,
		mGrassFullInstanceBuffer.UploadHeap);
	mGrassFullInstanceBuffer.mResource->SetName(L"GrassFullInstanceBuffer");
	mGrassFullInstanceBuffer.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;

	// CreateDefaultBuffer leaves the resource in GENERIC_READ, which includes
	// NON_PIXEL_SHADER_RESOURCE — no further transition needed for use as CS SRV.

	// 2. Visible instance buffer (UAV output of cull CS, read as SRV by VS)
	{
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(instBufSize,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(&mGrassVisibleBuffer.mResource)));
		mGrassVisibleBuffer.mResource->SetName(L"GrassVisibleBuffer");
	}

	// 3. Counter buffer (single uint, atomic increment target for cull CS)
	{
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT),
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(&mGrassCounterBuffer.mResource)));
		mGrassCounterBuffer.mResource->SetName(L"GrassCounterBuffer");
	}

	// 4. Counter reset buffer (upload heap, permanently holds UINT 0)
	//    Used every frame: CopyBufferRegion(counter, 0, counterReset, 0, 4)
	{
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
			IID_PPV_ARGS(&mGrassCounterResetBuffer.mResource)));
		mGrassCounterResetBuffer.mResource->SetName(L"GrassCounterResetBuffer");
		mGrassCounterResetBuffer.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;
		UINT* mapped = nullptr;
		ThrowIfFailed(mGrassCounterResetBuffer.mResource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
		*mapped = 0u;
		mGrassCounterResetBuffer.mResource->Unmap(0, nullptr);
	}

	// 5. Indirect args buffer (D3D12_DRAW_INDEXED_ARGUMENTS)
	//    Static fields (IndexCount, StartIndex, BaseVertex) set once here.
	//    InstanceCount is overwritten each frame from the counter buffer.
	{
		D3D12_DRAW_INDEXED_ARGUMENTS initArgs = {};
		initArgs.IndexCountPerInstance = mGrassRitem->IndexCount;
		initArgs.InstanceCount = 0;
		initArgs.StartIndexLocation = mGrassRitem->StartIndexLocation;
		initArgs.BaseVertexLocation = mGrassRitem->BaseVertexLocation;
		initArgs.StartInstanceLocation = 0;

		mGrassIndirectArgsBuffer.mResource = d3dUtil::CreateDefaultBuffer(
			device, cmdList,
			&initArgs, sizeof(initArgs),
			mGrassIndirectArgsBuffer.UploadHeap);  // kept as member — must outlive cmd flush
		mGrassIndirectArgsBuffer.mResource->SetName(L"GrassIndirectArgsBuffer");
		mGrassIndirectArgsBuffer.m_currState = D3D12_RESOURCE_STATE_GENERIC_READ;
		// CreateDefaultBuffer leaves it in GENERIC_READ; transition to
		// INDIRECT_ARGUMENT below alongside the other initial barriers.
	}

	// 6. CullCB upload buffer (ViewProj, EyePosW, DrawDistance, InstanceCount)
	//    Written every frame by MCEngine::GrassCullDispatch before dispatch.
	// mGrassCullCB = std::make_unique<UploadBuffer<GrassCullCB>>(device, 1, false);
	for (int i = 0; i < 3; i++)
		mGrassCullCB[i] = std::make_unique<UploadBuffer<GrassCullCB>>(device, 1, false);
	// 7. Initial resource state transitions
	//    GrassCullDispatch expects these states at the start of every frame:
	//      mGrassVisibleBuffer    — NON_PIXEL_SHADER_RESOURCE (transitions to UAV then back)
	//      mGrassCounterBuffer    — COPY_DEST (reset copy, then transitions to UAV then back)
	//      mGrassIndirectArgsBuffer — INDIRECT_ARGUMENT
	engine.GetBarrierManager().TransitionState(mGrassVisibleBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	engine.GetBarrierManager().TransitionState(mGrassCounterBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
	engine.GetBarrierManager().TransitionState(mGrassIndirectArgsBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	engine.GetBarrierManager().FlushBarriers(engine.GetCmdList());
}

// MCScene::ToJson passes a fresh json that becomes mod["params"] directly —
// write fields flat at the top of `j`. (The earlier double-wrapping `j["type"]`
// + `j["params"]` indirection produced { type, params: { type, params: { ... } } }
// on disk, which FromJson then read as the empty outer wrapper, silently
// resetting all module fields to defaults on every Reload.)
void MCGrassCullingModule::ToJson(nlohmann::json& j) const {
	j["grassCountWidth"] = grassCountWidth;
	j["grassCountDepth"] = grassCountDepth;
	j["grassCoverageWidth"] = grassCoverageWidth;
	j["grassCoverageDepth"] = grassCoverageDepth;
	j["grassRitemName"] = grassRitemName;
	j["planeRitemName"] = planeRitemName;
	j["numCellsX"] = numCellsX;
	j["numCellsZ"] = numCellsZ;
	j["useGpuCulling"] = useGpuCulling;
	j["equalColor"] = equalColor;
}

// FromJson receives the "params" object (not the full module entry) per Step 4c.
// `value(key, default)` returns the in-class default if the field is missing —
// covers older scene JSONs written before a field was added (forward compat).
void MCGrassCullingModule::FromJson(const nlohmann::json& j) {
	grassCountWidth    = j.value("grassCountWidth",    grassCountWidth);
	grassCountDepth    = j.value("grassCountDepth",    grassCountDepth);
	grassCoverageWidth = j.value("grassCoverageWidth", grassCoverageWidth);
	grassCoverageDepth = j.value("grassCoverageDepth", grassCoverageDepth);
	grassRitemName     = j.value("grassRitemName",     grassRitemName);
	planeRitemName     = j.value("planeRitemName",     planeRitemName);
	numCellsX          = j.value("numCellsX",          numCellsX);
	numCellsZ          = j.value("numCellsZ",          numCellsZ);
	useGpuCulling      = j.value("useGpuCulling",      useGpuCulling);
	equalColor         = j.value("equalColor",         equalColor);
}