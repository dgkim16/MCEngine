#include "MCEngine.h"
#include "Scene_Ch7.h"
#include "GeometryGenerator.h"
#include "modelLoader.h"
#include <DirectXColors.h>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Material → texture mapping for Scene_Ch7 
// MOVED TO Scene.h
// ---------------------------------------------------------------------------

/*
struct MatProps { std::string matName, textureName; int renderLevel; };
static const std::vector<MatProps> s_matProps = {
	{"woodCrate",  "woodCrateTex", 0},
	{"model",      "modelTex",     0},
	{"model_ref",  "modelTex",     2},
	{"gridFloor",  "gridTex",      0},
	{"water",      "waterTex",     3},
	{"fence",      "wirefenceTex", 4},
	{"mirror",     "iceTex",       1},
	{"brick",      "bricksTex",    0},
	{"treeSprites","treeArrTex",   6},
	{"tessellation","teapot_normal",7}
};
*/
// ---------------------------------------------------------------------------
void Scene_Ch7::Load(MCEngine& engine)
{
	BuildGeometry(engine);
	BuildSpriteGeometry(engine);
	BuildMaterials(engine);
	BuildRenderItems(engine);
}

void Scene_Ch7::Activate(MCEngine& engine)
{
	// Camera / lighting state is left at engine defaults.
	engine.SetModelRitem(mModelRitem);
	engine.SetReflectedModelRitem(mReflectedModelRitem);
	engine.SetTessellatedRitem(mTessellatedRitem);
}

// ---------------------------------------------------------------------------
void Scene_Ch7::BuildGeometry(MCEngine& engine)
{
	ID3D12Device*              device  = engine.GetDevice();
	ID3D12GraphicsCommandList* cmdList = engine.GetCmdList();

	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box      = geoGen.CreateBox(1.5f, 0.5f, 1.5f, 3);
	GeometryGenerator::MeshData grid     = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
	GeometryGenerator::MeshData sphere   = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);

	std::vector<Vertex>   loaded_vertices;
	std::vector<uint32_t> loaded_indices;
	const std::string filename = "Assets/Models/다람디.obj";
	if (!ModelLoader::LoadObjToVertexIndexBuffers(filename, loaded_vertices, loaded_indices))
		MessageBoxA(nullptr, "Scene_Ch7: Failed to load OBJ", "Error", MB_OK);

	GeometryGenerator::MeshData quad = geoGen.CreateQuadPatch(-1.0f, -1.0f, 2, 2, 0.0f);

	UINT boxVertexOffset      = 0;
	UINT gridVertexOffset     = (UINT)box.Vertices.size();
	UINT sphereVertexOffset   = gridVertexOffset  + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();

	UINT boxIndexOffset      = 0;
	UINT gridIndexOffset     = (UINT)box.Indices32.size();
	UINT sphereIndexOffset   = gridIndexOffset  + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount       = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;
	boxSubmesh.CreateBounds(box.Vertices);

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount        = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;
	gridSubmesh.CreateBounds(grid.Vertices);

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount        = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;
	sphereSubmesh.CreateBounds(sphere.Vertices);

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount        = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;
	cylinderSubmesh.CreateBounds(cylinder.Vertices);

	SubmeshGeometry modelSubmesh;
	modelSubmesh.IndexCount        = (UINT)loaded_indices.size();
	modelSubmesh.StartIndexLocation = cylinderIndexOffset + (UINT)cylinder.Indices32.size();
	modelSubmesh.BaseVertexLocation = cylinderVertexOffset + (UINT)cylinder.Vertices.size();
	modelSubmesh.CreateBounds(loaded_vertices);

	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount        = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = modelSubmesh.StartIndexLocation + (UINT)loaded_indices.size();
	quadSubmesh.BaseVertexLocation = modelSubmesh.BaseVertexLocation + (UINT)loaded_vertices.size();
	quadSubmesh.CreateBounds(quad.Vertices);

	auto totalVertexCount =
		box.Vertices.size() + grid.Vertices.size() +
		sphere.Vertices.size() + cylinder.Vertices.size() +
		loaded_vertices.size() + quad.Vertices.size();
	std::vector<Vertex> vertices(totalVertexCount);

	UINT k = 0;
	for (size_t i = 0; i < box.Vertices.size();      ++i, ++k) { vertices[k].Pos = box.Vertices[i].Position;      vertices[k].Normal = box.Vertices[i].Normal;      vertices[k].TexC = box.Vertices[i].TexC; }
	for (size_t i = 0; i < grid.Vertices.size();     ++i, ++k) { vertices[k].Pos = grid.Vertices[i].Position;     vertices[k].Normal = grid.Vertices[i].Normal;     vertices[k].TexC = grid.Vertices[i].TexC; }
	for (size_t i = 0; i < sphere.Vertices.size();   ++i, ++k) { vertices[k].Pos = sphere.Vertices[i].Position;   vertices[k].Normal = sphere.Vertices[i].Normal;   vertices[k].TexC = sphere.Vertices[i].TexC; }
	for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k) { vertices[k].Pos = cylinder.Vertices[i].Position; vertices[k].Normal = cylinder.Vertices[i].Normal; vertices[k].TexC = cylinder.Vertices[i].TexC; }
	for (size_t i = 0; i < loaded_vertices.size();   ++i, ++k) { vertices[k].Pos = loaded_vertices[i].Pos;        vertices[k].Normal = loaded_vertices[i].Normal;   vertices[k].TexC = loaded_vertices[i].TexC; }
	for (size_t i = 0; i < quad.Vertices.size();     ++i, ++k) { vertices[k].Pos = quad.Vertices[i].Position;     vertices[k].Normal = quad.Vertices[i].Normal;     vertices[k].TexC = quad.Vertices[i].TexC; }

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()),      std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()),     std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()),   std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
	for (uint32_t idx : loaded_indices) {
		if (idx > UINT16_MAX) throw std::runtime_error("Index value exceeds uint16_t range.");
		indices.push_back(static_cast<std::uint16_t>(idx));
	}
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";
	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(device, cmdList, indices.data(),  ibByteSize, geo->IndexBufferUploader);
	geo->VertexByteStride   = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat        = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;
	geo->DrawArgs["box"]      = boxSubmesh;
	geo->DrawArgs["grid"]     = gridSubmesh;
	geo->DrawArgs["sphere"]   = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;
	geo->DrawArgs["model"]    = modelSubmesh;
	geo->DrawArgs["quad"]     = quadSubmesh;
	geometries[geo->Name] = std::move(geo);
}

// ---------------------------------------------------------------------------
void Scene_Ch7::BuildSpriteGeometry(MCEngine& engine)
{
	ID3D12Device*              device  = engine.GetDevice();
	ID3D12GraphicsCommandList* cmdList = engine.GetCmdList();

	struct TreeSpriteVertex { XMFLOAT3 Pos; XMFLOAT2 Size; };
	static const int treeCount = 16;
	std::array<TreeSpriteVertex, 16> vertices;
	for (UINT i = 0; i < treeCount; ++i) {
		float x = MathHelper::RandF(-45.0f, 45.0f);
		float z = MathHelper::RandF(-45.0f, 45.0f);
		if (std::abs(x) < 20.0f && std::abs(z) < 20.0f) {
			x = 20.0f * x / std::abs(x);
			z = 20.0f * z / std::abs(z);
		}
		vertices[i].Pos  = XMFLOAT3(x, 8.0f, z);
		vertices[i].Size = XMFLOAT2(20.0f, 20.0f);
	}
	std::array<std::uint16_t, 16> indices = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(TreeSpriteVertex);
	const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "treeSpritesGeo";
	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);
	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(device, cmdList, indices.data(),  ibByteSize, geo->IndexBufferUploader);
	geo->VertexByteStride    = sizeof(TreeSpriteVertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;
	SubmeshGeometry submesh;
	submesh.IndexCount        = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	geo->DrawArgs["points"] = submesh;
	geometries["treeSpritesGeo"] = std::move(geo);
}

// ---------------------------------------------------------------------------
void Scene_Ch7::BuildMaterials(MCEngine& engine)
{
	int i = 0;
	for (const auto& mp : s_matProps) {
		auto mat = std::make_unique<Material>();
		mat->Name         = mp.matName;
		mat->textureName  = mp.textureName;  // stored for FixupMaterialDiffuseIndices()
		mat->MatCBIndex   = i++;
		{ auto* t = engine.GetTexture(mp.textureName); mat->DiffuseSrvHeapIndex = t->SRVs.empty() ? 0 : t->SRVs[0].offset; } // may be 0 until fixup
		mat->FresnelR0    = XMFLOAT3(0.05f, 0.05f, 0.05f);
		mat->Roughness    = 0.2f;
		mat->renderLevel  = mp.renderLevel;
		mat->NumFramesDirty = gNumFrameResources;
		materials[mat->Name] = std::move(mat);
	}
	int q = 0;
	for (auto& e : materials)
		materialIndexTracker[q++] = e.first;
}

// ---------------------------------------------------------------------------
void Scene_Ch7::BuildRenderItems(MCEngine& engine)
{
	UINT objCBIndex = 0;

	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
	boxRitem->ObjCBIndex = objCBIndex++;
	boxRitem->Name = "box";
	boxRitem->Geo  = geometries["shapeGeo"].get();
	boxRitem->Mat  = materials["woodCrate"].get();
	boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount        = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(boxRitem.get());
	allRitems.push_back(std::move(boxRitem));

	auto modelRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&modelRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f) * XMMatrixTranslation(0.0f, 2.0f, 0.0f));
	modelRitem->ObjCBIndex = objCBIndex++;
	modelRitem->Name = "model";
	modelRitem->Geo  = geometries["shapeGeo"].get();
	modelRitem->Mat  = materials["model"].get();
	modelRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	modelRitem->IndexCount        = modelRitem->Geo->DrawArgs["model"].IndexCount;
	modelRitem->StartIndexLocation = modelRitem->Geo->DrawArgs["model"].StartIndexLocation;
	modelRitem->BaseVertexLocation = modelRitem->Geo->DrawArgs["model"].BaseVertexLocation;
	modelRitem->Bounds = modelRitem->Geo->DrawArgs["model"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(modelRitem.get());
	engine.SetModelRitem(modelRitem.get());
	mModelRitem = modelRitem.get();

	auto reflectedModelItem = std::make_unique<RenderItem>();
	*reflectedModelItem = *modelRitem;
	reflectedModelItem->ObjCBIndex = objCBIndex++;
	reflectedModelItem->Mat = materials["model_ref"].get();
	XMStoreFloat4x4(&reflectedModelItem->World, XMMatrixTranslation(.0f, 2.0f, -10.0f));
	engine.SetReflectedModelRitem(reflectedModelItem.get());
	mReflectedModelRitem = reflectedModelItem.get();
	layers[(int)RenderLayer::Reflected].insert(reflectedModelItem.get());

	allRitems.push_back(std::move(modelRitem));
	allRitems.push_back(std::move(reflectedModelItem));

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	gridRitem->ObjCBIndex = objCBIndex++;
	gridRitem->Name = "grid";
	gridRitem->Geo  = geometries["shapeGeo"].get();
	gridRitem->Mat  = materials["gridFloor"].get();
	gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount        = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(gridRitem.get());
	allRitems.push_back(std::move(gridRitem));

	auto quadRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&quadRitem->World, XMMatrixRotationX(MathHelper::Pi / 2.0f) * XMMatrixTranslation(0.0f, 3.0f, 0.0f));
	XMStoreFloat4x4(&quadRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	quadRitem->ObjCBIndex = objCBIndex++;
	quadRitem->Name = "quad";
	quadRitem->Geo  = geometries["shapeGeo"].get();
	quadRitem->Mat  = materials["tessellation"].get();
	quadRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
	quadRitem->IndexCount        = quadRitem->Geo->DrawArgs["quad"].IndexCount;
	quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
	quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
	quadRitem->Bounds = quadRitem->Geo->DrawArgs["quad"].Bounds;
	engine.SetTessellatedRitem(quadRitem.get());
	mTessellatedRitem = quadRitem.get();
	layers[(int)RenderLayer::OpaqueTessellated].insert(quadRitem.get());
	allRitems.push_back(std::move(quadRitem));

	auto mirrorRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&mirrorRitem->World, XMMatrixScaling(.5f, 1.0f, .25f) * XMMatrixRotationX(MathHelper::Pi / 2.0f) * XMMatrixTranslation(0.0f, 3.0f, -5.0f));
	mirrorRitem->ObjCBIndex = objCBIndex++;
	mirrorRitem->Name = "mirror";
	mirrorRitem->Geo  = geometries["shapeGeo"].get();
	mirrorRitem->Mat  = materials["mirror"].get();
	mirrorRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	mirrorRitem->IndexCount        = mirrorRitem->Geo->DrawArgs["grid"].IndexCount;
	mirrorRitem->StartIndexLocation = mirrorRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	mirrorRitem->BaseVertexLocation = mirrorRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	mirrorRitem->Bounds = mirrorRitem->Geo->DrawArgs["grid"].Bounds;
	layers[(int)RenderLayer::Mirrors].insert(mirrorRitem.get());
	layers[(int)RenderLayer::Transparent].insert(mirrorRitem.get());
	allRitems.push_back(std::move(mirrorRitem));

	auto waterRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&waterRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
	XMStoreFloat4x4(&waterRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	waterRitem->ObjCBIndex = objCBIndex++;
	waterRitem->Name = "water";
	waterRitem->Geo  = geometries["shapeGeo"].get();
	waterRitem->Mat  = materials["water"].get();
	waterRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	waterRitem->IndexCount        = waterRitem->Geo->DrawArgs["grid"].IndexCount;
	waterRitem->StartIndexLocation = waterRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	waterRitem->BaseVertexLocation = waterRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	waterRitem->Bounds = waterRitem->Geo->DrawArgs["grid"].Bounds;
	layers[(int)RenderLayer::Transparent].insert(waterRitem.get());
	allRitems.push_back(std::move(waterRitem));

	auto treeSpritesRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&treeSpritesRitem->World, XMMatrixScaling(0.1f, .1f, .1f));
	treeSpritesRitem->ObjCBIndex = objCBIndex++;
	treeSpritesRitem->Mat = materials["treeSprites"].get();
	treeSpritesRitem->Geo = geometries["treeSpritesGeo"].get();
	treeSpritesRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
	treeSpritesRitem->IndexCount        = treeSpritesRitem->Geo->DrawArgs["points"].IndexCount;
	treeSpritesRitem->StartIndexLocation = treeSpritesRitem->Geo->DrawArgs["points"].StartIndexLocation;
	treeSpritesRitem->BaseVertexLocation = treeSpritesRitem->Geo->DrawArgs["points"].BaseVertexLocation;
	treeSpritesRitem->checkBounds = false;
	layers[(int)RenderLayer::AlphaTestedTreeSprites].insert(treeSpritesRitem.get());
	allRitems.push_back(std::move(treeSpritesRitem));

	for (int i = 0; i < 5; ++i) {
		auto leftCylRitem    = std::make_unique<RenderItem>();
		auto rightCylRitem   = std::make_unique<RenderItem>();
		auto leftSphereRitem  = std::make_unique<RenderItem>();
		auto rightSphereRitem = std::make_unique<RenderItem>();

		XMMATRIX leftCylWorld    = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
		XMMATRIX rightCylWorld   = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);
		XMMATRIX leftSphereWorld  = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
		XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

		XMStoreFloat4x4(&leftCylRitem->World, leftCylWorld);
		leftCylRitem->ObjCBIndex = objCBIndex++;
		leftCylRitem->Name = "leftCylRitem";
		leftCylRitem->Geo  = geometries["shapeGeo"].get();
		leftCylRitem->Mat  = materials["woodCrate"].get();
		leftCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		leftCylRitem->IndexCount        = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
		leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
		leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
		leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;

		XMStoreFloat4x4(&rightCylRitem->World, rightCylWorld);
		rightCylRitem->ObjCBIndex = objCBIndex++;
		rightCylRitem->Name = "rightCylRitem";
		rightCylRitem->Geo  = geometries["shapeGeo"].get();
		rightCylRitem->Mat  = materials["woodCrate"].get();
		rightCylRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		rightCylRitem->IndexCount        = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
		rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
		rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
		rightCylRitem->Bounds = rightCylRitem->Geo->DrawArgs["cylinder"].Bounds;

		XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
		leftSphereRitem->ObjCBIndex = objCBIndex++;
		leftSphereRitem->Name = "leftSphereRitem";
		leftSphereRitem->Geo  = geometries["shapeGeo"].get();
		leftSphereRitem->Mat  = materials["woodCrate"].get();
		leftSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		leftSphereRitem->IndexCount        = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		leftSphereRitem->Bounds = leftSphereRitem->Geo->DrawArgs["sphere"].Bounds;

		XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
		rightSphereRitem->ObjCBIndex = objCBIndex++;
		rightSphereRitem->Name = "rightSphereRitem";
		rightSphereRitem->Geo  = geometries["shapeGeo"].get();
		rightSphereRitem->Mat  = materials["woodCrate"].get();
		rightSphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		rightSphereRitem->IndexCount        = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;

		layers[(int)RenderLayer::Opaque].insert(leftCylRitem.get());
		layers[(int)RenderLayer::Opaque].insert(rightCylRitem.get());
		layers[(int)RenderLayer::Opaque].insert(leftSphereRitem.get());
		layers[(int)RenderLayer::Opaque].insert(rightSphereRitem.get());
		allRitems.push_back(std::move(leftCylRitem));
		allRitems.push_back(std::move(rightCylRitem));
		allRitems.push_back(std::move(leftSphereRitem));
		allRitems.push_back(std::move(rightSphereRitem));
	}
}
