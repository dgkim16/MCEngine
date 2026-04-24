#include "Scene_grass.h"
#include "ShaderLib.h"
#include "MCEngine.h"
#include "imgui.h"
#include <random>
#include <numeric>

void Scene_grass::Load(MCEngine& engine) {
	BuildGeometry(engine);
	BuildMaterials(engine);
	BuildRenderItems(engine);
	BuildGpuCullingBuffers(engine);
}

void Scene_grass::Activate(MCEngine& engine) {
	engine.SetGrassCullingScene(this);
}

void Scene_grass::Deactivate(MCEngine& engine) {
	OutputDebugString(L"Deactivate Scene_grass\n");
	engine.SetGrassCullingScene(nullptr);
}

void Scene_grass::Update(MCEngine& engine, float dt) {
}

void Scene_grass::ResetSceneResources() {
	mGrassFullInstanceBuffer = {}; // this is ok because they don't have any Views assigned (vector)
	mGrassIndirectArgsBuffer = {}; // if they do get views, this won't cut it - would need...
	mGrassVisibleBuffer = {};	   // ... to call DescHeapManager::QueueRemoval_Buffer
	mGrassCounterBuffer = {};
	mGrassCounterResetBuffer = {};

	mGrassCullCB = {};
}

//! NOTE: this is unique for each scene. Would be better if there is a separate class managaing all simple geometries
void Scene_grass::BuildGeometry(MCEngine& engine) { 

	ID3D12Device* device = engine.GetDevice();
	ID3D12GraphicsCommandList* cmdList = engine.GetCmdList();

	GeometryGenerator geoGen;
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(grassCoverageWidth, grassCoverageDepth, 2, 2); // plane
	// GeometryGenerator::MeshData quad = geoGen.CreateQuad(-0.5f, 1.0f, 1.0f, 1.0f, 0.0f); // single quad for single grass
	// GeometryGenerator::MeshData quad = geoGen.CreateGrassTriangle(grassWidth, grassHeight);
	GeometryGenerator::MeshData quad = geoGen.CreateGrassPatch(grassWidth, grassHeight, grassSharpness);
	std::vector<Vertex>   loaded_vertices;
	std::vector<uint32_t> loaded_indices;

	UINT gridVertexOffset = 0;
	UINT quadVertexOffset = (UINT)grid.Vertices.size();

	UINT gridIndexOffset = 0;
	UINT quadIndexOffset = (UINT)grid.Indices32.size();

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;
	gridSubmesh.CreateBounds(grid.Vertices);

	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = quadIndexOffset;
	quadSubmesh.BaseVertexLocation = quadVertexOffset;
	quadSubmesh.CreateBounds(quad.Vertices);

	auto totalVertexCount = grid.Vertices.size() + quad.Vertices.size();
	std::vector<Vertex> vertices(totalVertexCount);
	UINT k = 0;
	for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k) { vertices[k].Pos = grid.Vertices[i].Position;     vertices[k].Normal = grid.Vertices[i].Normal;     vertices[k].TexC = grid.Vertices[i].TexC; }
	for (size_t i = 0; i < quad.Vertices.size(); ++i, ++k) { vertices[k].Pos = quad.Vertices[i].Position;     vertices[k].Normal = quad.Vertices[i].Normal;     vertices[k].TexC = quad.Vertices[i].TexC; }

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";
	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, indices.data(), ibByteSize, geo->IndexBufferUploader);
	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["quad"] = quadSubmesh;
	geometries[geo->Name] = std::move(geo);
}

void Scene_grass::BuildMaterials(MCEngine& engine) {
	int i = 0;
	for (const auto& mp : s_matProps) {
		auto mat = std::make_unique<Material>();
		mat->Name = mp.matName;
		mat->textureName = mp.textureName;  // stored for FixupMaterialDiffuseIndices()
		mat->MatCBIndex = i++;
		{
			auto* t = engine.GetTexture(mp.textureName);
			mat->DiffuseSrvHeapIndex = t->SRVs.empty() ? 0 : t->SRVs[0].offset; // may be 0 until fixup
		}
		mat->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
		mat->Roughness = 0.2f;
		mat->renderLevel = mp.renderLevel;
		mat->NumFramesDirty = gNumFrameResources;
		
		materials[mat->Name] = std::move(mat);
	}
	int q = 0;
	for (auto& e : materials) {
		std::cout << e.first << " is at index " << q << std::endl;
		materialIndexTracker[q++] = e.first;
	}
	mGrassMaterial = materials["grass"].get();
	mPlaneMaterial = materials["grassPlane"].get();

}

void Scene_grass::BuildRenderItems(MCEngine& engine) {
	UINT objCBIndex = 0;
	UINT objInstIndex = 0;
	float w = grassCoverageWidth;
	float d = grassCoverageDepth;
	UINT grassCount = grassCountWidth * grassCountDepth;

	OutputDebugString(L"start gridRitem\n");
	auto gridRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&gridRitem->World, XMMatrixTranslation(.0f, -0.01f, .0f));
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	gridRitem->ObjCBIndex = objCBIndex++;
	gridRitem->Name = "grid";
	gridRitem->Geo = geometries["shapeGeo"].get();
	gridRitem->Mat = materials["grassPlane"].get();
	gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(gridRitem.get());
	mPlaneRitem = gridRitem.get();
	allRitems.push_back(std::move(gridRitem));

	OutputDebugString(L"start quadRitem\n");
	auto quadRitem = std::make_unique<RenderItem>();
	quadRitem->ObjInstIndex = objInstIndex++;
	quadRitem->ObjCBIndex = objCBIndex++;
	quadRitem->Name = "quad";
	quadRitem->Geo = geometries["shapeGeo"].get();
	quadRitem->Mat = materials["grass"].get();
	// quadRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	quadRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
	quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
	quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
	quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
	quadRitem->Bounds = quadRitem->Geo->DrawArgs["quad"].Bounds;
	quadRitem->GrassInstances.resize(grassCount);
	OutputDebugString(L"started instancing render item\n");

	std::mt19937 rng(12345);
	std::uniform_real_distribution<float> jitterDist(-0.4f, 0.4f);
	std::uniform_real_distribution<float> rotDist(0.0f, XM_2PI);
	std::uniform_real_distribution<float> scaleDist(0.85f, 1.15f);

	float x = -0.5f * w;
	float z = -0.5f * d;

	float cellW = w / grassCountWidth;
	float cellD = d / grassCountDepth;

	float dx = w / (grassCountWidth - 1);
	float dz = d / (grassCountDepth - 1);

	for (int k = 0; k < grassCountDepth; k++) 
	{
		for (int i = 0; i < grassCountWidth; i++) {
			int index = k * grassCountWidth + i;
			float px = x + (i + 0.5f) * cellW + jitterDist(rng) * cellW; 
			float pz = z + (k + 0.5f) * cellD + jitterDist(rng) * cellD; // randomize for every grass, not per depth
			float rotY = rotDist(rng);
			float scale = scaleDist(rng);
			XMMATRIX world =
				XMMatrixScaling(scale, scale, scale) *
				XMMatrixRotationY(rotY) *
				XMMatrixTranslation(px, 0.0f, pz);
			auto posfl3 = XMFLOAT3(px, 0.0f, pz);
			XMVECTOR position = XMLoadFloat3(&posfl3);
			XMStoreFloat3(&quadRitem->GrassInstances[index].grassPosition, position);
			quadRitem->GrassInstances[index].cosYaw = cos(rotY);
			quadRitem->GrassInstances[index].sinYaw = sin(rotY);
			quadRitem->GrassInstances[index].scale = scale;
			// XMStoreFloat4x4(&quadRitem->GrassInstances[index].World, world);
		}
	}
	OutputDebugString(L"completed building render item!\n");

	layers[(int)RenderLayer::GrassInstanced].insert(quadRitem.get());
	mGrassRitem = quadRitem.get();
	mGrassRitem->useCellCulling = true;
	allRitems.push_back(std::move(quadRitem));

	// this is needed if we want to do CPU culling
	BuildInstanceCells();
}

void Scene_grass::BuildInstanceCells() {
	float w = grassCoverageWidth;
	float d = grassCoverageDepth;
	float cellFootW = w / numCellsX;
	float cellFootD = d / numCellsZ;

	// Expand cell AABB slightly beyond footprint to account for blade radius + scale
	float bladeHalfW = grassWidth * 1.15f * 0.5f;
	float bladeHeight = grassHeight * 1.15f;

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

void Scene_grass::BuildGpuCullingBuffers(MCEngine& engine) {
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

	// transposed no longer needed as matrix is not used
	/*
	std::vector<GrassInstanceData> transposed(mTotalGrassInstances);
	for (UINT i = 0; i < mTotalGrassInstances; i++) {
		// XMMATRIX world = XMLoadFloat4x4(&mGrassRitem->GrassInstances[i].World);
		// XMStoreFloat4x4(&transposed[i].World,XMMatrixTranspose(world));
		XMVECTOR position = XMLoadFloat3(&mGrassRitem->GrassInstances[i].grassPosition);
	}
	*/
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
		initArgs.InstanceCount         = 0;
		initArgs.StartIndexLocation    = mGrassRitem->StartIndexLocation;
		initArgs.BaseVertexLocation    = mGrassRitem->BaseVertexLocation;
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
	CD3DX12_RESOURCE_BARRIER initBarriers[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(
			mGrassVisibleBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(
			mGrassCounterBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_COPY_DEST),
		CD3DX12_RESOURCE_BARRIER::Transition(
			mGrassIndirectArgsBuffer.mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
	};
	cmdList->ResourceBarrier(_countof(initBarriers), initBarriers);
}



void Scene_grass::Scene_IMGUI(MCEngine& engine) {
	if (ImGui::Button("Rebuild Scene"))
		engine.RequestReload();
	ImGui::Separator();
	ImGui::Checkbox("GPU Culling (Compute Shader)", &useGpuCulling);
	if (useGpuCulling) {
		ImGui::TextDisabled("Visible count: GPU-side only");
		ImGui::TextDisabled("Cell culling disabled in GPU mode");
	} else {
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
		// if (rebuildCells) engine.RequestReload();
	}
	ImGui::Text("%.2f, %.2f, %.2f", grassWidth, grassHeight, grassSharpness);
	ImGui::SliderFloat("grass sharpness", &grassSharpness, 0.01f, 1.0f);
	ImGui::SliderFloat("grass Width", &grassWidth, 0.01f, 1.0f);
	ImGui::SliderFloat("grass Height", &grassHeight, 0.01f, 1.0f);
	ImGui::Separator();
	ImGui::Text("%d, %d", grassCountWidth, grassCountDepth);
	ImGui::SliderInt("grassCountWidth", &grassCountWidth, 1, 1000);
	ImGui::SliderInt("grassCountDepth", &grassCountDepth, 1, 1000);
	ImGui::Separator();
	ImGui::Text("%.2f, %.2f", grassCoverageWidth, grassCoverageDepth);
	ImGui::SliderFloat("grassCoverageWidth", &grassCoverageWidth, 1.0f, 1000.0f);
	ImGui::SliderFloat("grassCoverageDepth", &grassCoverageDepth, 1.0f, 1000.0f);
	ImGui::Separator();
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
			XMStoreFloat4(&mPlaneMaterial->DiffuseAlbedo,result);
			mPlaneMaterial->NumFramesDirty = gNumFrameResources;
		}
	}
	if (equalColor)
		return;
	XMFLOAT4& colorPlane = mPlaneMaterial->DiffuseAlbedo;
	if(ImGui::ColorPicker3("plane color", &colorPlane.x, 0))
		mPlaneMaterial->NumFramesDirty = gNumFrameResources;
	
}