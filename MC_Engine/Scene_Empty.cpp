#include "MCEngine.h"
#include "Scene_Empty.h"
#include "GeometryGenerator.h"

using namespace DirectX;

void Scene_Empty::Load(MCEngine& engine)
{
	/*
	ID3D12Device*              device  = engine.GetDevice();
	ID3D12GraphicsCommandList* cmdList = engine.GetCmdList();

	// --- Geometry ---
	GeometryGenerator geoGen;
	GeometryGenerator::MeshData box  = geoGen.CreateBox(1.5f, 0.5f, 1.5f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);

	UINT boxVertexOffset  = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT boxIndexOffset   = 0;
	UINT gridIndexOffset  = (UINT)box.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount        = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;
	boxSubmesh.CreateBounds(box.Vertices);

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount        = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;
	gridSubmesh.CreateBounds(grid.Vertices);

	auto totalCount = box.Vertices.size() + grid.Vertices.size();
	std::vector<Vertex> vertices(totalCount);
	UINT k = 0;
	for (size_t i = 0; i < box.Vertices.size();  ++i, ++k) { vertices[k].Pos = box.Vertices[i].Position;  vertices[k].Normal = box.Vertices[i].Normal;  vertices[k].TexC = box.Vertices[i].TexC; }
	for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k) { vertices[k].Pos = grid.Vertices[i].Position; vertices[k].Normal = grid.Vertices[i].Normal; vertices[k].TexC = grid.Vertices[i].TexC; }

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()),  std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "emptyGeo";
	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(device, cmdList, indices.data(),  ibByteSize, geo->IndexBufferUploader);
	geo->VertexByteStride    = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;
	geo->DrawArgs["box"]  = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geometries["emptyGeo"] = std::move(geo);

	// --- Materials (2: one for box, one for grid) ---
	{
		auto mat = std::make_unique<Material>();
		mat->Name        = "emptyBox";
		mat->textureName = "default";
		mat->MatCBIndex  = 0;
		{ auto* t = engine.GetTexture("default"); mat->DiffuseSrvHeapIndex = t->SRVs.empty() ? 0 : t->SRVs[0].offset; }
		mat->FresnelR0   = XMFLOAT3(0.05f, 0.05f, 0.05f);
		mat->Roughness   = 0.3f;
		mat->renderLevel = 0;
		mat->NumFramesDirty = gNumFrameResources;
		materials["emptyBox"] = std::move(mat);
	}
	{
		auto mat = std::make_unique<Material>();
		mat->Name        = "emptyGrid";
		mat->textureName = "gridTex";
		mat->MatCBIndex  = 1;
		{ auto* t = engine.GetTexture("gridTex"); mat->DiffuseSrvHeapIndex = t->SRVs.empty() ? 0 : t->SRVs[0].offset; }
		mat->FresnelR0   = XMFLOAT3(0.02f, 0.02f, 0.02f);
		mat->Roughness   = 0.8f;
		mat->renderLevel = 0;
		mat->NumFramesDirty = gNumFrameResources;
		materials["emptyGrid"] = std::move(mat);
	}
	int q = 0;
	for (auto& e : materials) materialIndexTracker[q++] = e.first;

	// --- Render items ---
	UINT objCBIndex = 0;

	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
	boxRitem->ObjCBIndex = objCBIndex++;
	boxRitem->Name = "emptyBox";
	boxRitem->Geo  = geometries["emptyGeo"].get();
	boxRitem->Mat  = materials["emptyBox"].get();
	boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount        = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(boxRitem.get());
	allRitems.push_back(std::move(boxRitem));

	auto gridRitem = std::make_unique<RenderItem>();
	gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
	gridRitem->ObjCBIndex = objCBIndex++;
	gridRitem->Name = "emptyGrid";
	gridRitem->Geo  = geometries["emptyGeo"].get();
	gridRitem->Mat  = materials["emptyGrid"].get();
	gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	gridRitem->IndexCount        = gridRitem->Geo->DrawArgs["grid"].IndexCount;
	gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
	gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;
	layers[(int)RenderLayer::Opaque].insert(gridRitem.get());
	allRitems.push_back(std::move(gridRitem));
	*/
}

void Scene_Empty::Activate(MCEngine& engine)
{
	// No special camera or lighting needed; defaults are fine.
}
