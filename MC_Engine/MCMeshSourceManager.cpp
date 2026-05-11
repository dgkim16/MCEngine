#include "MCMeshSourceManager.h"
#include "MCEngine.h"
#include "MCSceneManager.h"              // RecordAssetRedirect (Step 9c)
#include "modelLoader.h"                 // ModelLoader::LoadObjToVertexIndexBuffers (ImportedFromFile kind)
#include "Common/GeometryGenerator.h"   // GeometryGenerator::Create*, MeshData
#include "Common/MathHelper.h"           // MathHelper::RandF (RandomSpritePoints kind)
#include "Common/d3dUtil.h"              // d3dUtil::CreateDefaultBuffer, ThrowIfFailed

#include <d3dcompiler.h>                 // D3DCreateBlob
#include <DirectXMath.h>                 // XMFLOAT3 / XMFLOAT2 for TreeSpriteVertex
#include <numeric>                       // std::iota for sprite-point indices
#include <stdexcept>

const std::set<std::string> MCMeshSourceManager::kEmptyScenes;

MCMeshSourceManager::MCMeshSourceManager(MCEngine& engine) : mEngine(engine) {}

// Register a mesh source. Three valid paths by source.kind:
//   - Procedural_*: source.geometry MUST be null. The manager builds the
//     MCMeshGeometry via BuildProcedural and assigns it.
//   - External / ImportedFromFile: source.geometry MUST be non-null (caller
//     pre-built it; for ImportedFromFile this means the caller already drove
//     ModelLoader before calling Register).
// Idempotent on duplicate displayName with matching kind. Throws on kind
// mismatch (caller bug — same name, different mesh).
MeshHandle MCMeshSourceManager::Register(const std::string& displayName, MCMeshSource source)
{
	auto h = HashAssetIdentity<AssetKind::MeshSource>(displayName);

	if (auto it = mSources.find(h); it != mSources.end()) {
		if (it->second.kind != source.kind) {
			throw std::runtime_error(
				"MCMeshSourceManager::Register: displayName '" + displayName +
				"' already registered with a different kind");
		}
		return h;
	}

	// Every kind under the v2 (drop-merging) model is built by the manager
	// from (kind, params); no caller pre-builds geometry. The pre-built path
	// (formerly Kind::External + ImportedFromFile-with-pre-built) is gone.
	if (source.geometry) {
		throw std::runtime_error(
			"MCMeshSourceManager::Register: source '" + displayName +
			"' must NOT carry a pre-built geometry; manager builds it from kind+params.");
	}
	source.geometry = BuildProcedural(displayName, source);

	mSources[h] = std::move(source);
	return h;
}

// Procedural build dispatch. Calls the right GeometryGenerator::Create*,
// converts GeometryGenerator::Vertex → MCVertex (we drop TangentU; MCVertex
// is Pos/Normal/TexC only), uploads VB and IB into default-heap buffers
// via d3dUtil::CreateDefaultBuffer, and assembles the MCMeshGeometry with
// a single DrawArgs entry covering the whole buffer.
//
// PRECONDITION: the engine's command list is open. CreateDefaultBuffer
// records copy commands into it; the caller (MCScene::Load via engine init)
// batches close/execute/flush after all sources are registered, so a single
// fence wait covers all uploads.
std::unique_ptr<MCMeshGeometry> MCMeshSourceManager::BuildProcedural(
	const std::string& displayName, const MCMeshSource& s)
{
	// RandomSpritePoints emits TreeSpriteVertex (Pos+Size) at sizeof(TreeSpriteVertex)
	// stride — different from every other kind. Its assembly is fully separate.
	if (s.kind == MCMeshSource::Kind::Procedural_RandomSpritePoints)
		return BuildRandomSpritePoints(displayName, std::get<MCMeshSource::RandomSpritePointsParams>(s.params));

	GeometryGenerator gen;
	GeometryGenerator::MeshData meshData;

	// Two ways to fill (vertices, indices):
	//   - GeometryGenerator-backed kinds set meshData; the post-switch block
	//     converts MeshData::Vertex → MCVertex (TangentU dropped) and reads
	//     uint16 indices via meshData.GetIndices16().
	//   - ImportedFromFile fills vertices/indices directly via ModelLoader and
	//     flips meshDataFilled to skip the post-switch conversion.
	std::vector<MCVertex>      vertices;
	std::vector<std::uint16_t> indices;
	bool meshDataFilled = true;

	switch (s.kind) {
	case MCMeshSource::Kind::Procedural_Box: {
		const auto& p = std::get<MCMeshSource::BoxParams>(s.params);
		meshData = gen.CreateBox(p.w, p.h, p.d, (std::uint32_t)p.subdivisions);
		break;
	}
	case MCMeshSource::Kind::Procedural_Sphere: {
		const auto& p = std::get<MCMeshSource::SphereParams>(s.params);
		meshData = gen.CreateSphere(p.radius, (std::uint32_t)p.slices, (std::uint32_t)p.stacks);
		break;
	}
	case MCMeshSource::Kind::Procedural_Cylinder: {
		const auto& p = std::get<MCMeshSource::CylinderParams>(s.params);
		meshData = gen.CreateCylinder(p.bottomRadius, p.topRadius, p.height,
		                              (std::uint32_t)p.slices, (std::uint32_t)p.stacks);
		break;
	}
	case MCMeshSource::Kind::Procedural_Grid: {
		const auto& p = std::get<MCMeshSource::GridParams>(s.params);
		meshData = gen.CreateGrid(p.w, p.d, (std::uint32_t)p.m, (std::uint32_t)p.n);
		break;
	}
	case MCMeshSource::Kind::Procedural_GeoSphere: {
		const auto& p = std::get<MCMeshSource::GeoSphereParams>(s.params);
		meshData = gen.CreateGeosphere(p.radius, (std::uint32_t)p.subdivisions);
		break;
	}
	case MCMeshSource::Kind::Procedural_Quad: {
		const auto& p = std::get<MCMeshSource::QuadParams>(s.params);
		meshData = gen.CreateQuadPatch(p.x, p.y, p.w, p.h, p.z);
		break;
	}
	case MCMeshSource::Kind::Procedural_GrassPatch: {
		const auto& p = std::get<MCMeshSource::GrassPatchParams>(s.params);
		meshData = gen.CreateGrassPatch(p.width, p.height, p.sharpness);
		break;
	}
	case MCMeshSource::Kind::ImportedFromFile: {
		const auto& p = std::get<MCMeshSource::ImportedFromFileParams>(s.params);
		std::vector<std::uint32_t> wideIndices;
		if (!ModelLoader::LoadObjToVertexIndexBuffers(p.path, vertices, wideIndices))
			throw std::runtime_error("ImportedFromFile: load failed for '" + p.path + "'");
		indices.reserve(wideIndices.size());
		for (auto idx : wideIndices) {
			if (idx > UINT16_MAX)
				throw std::runtime_error("ImportedFromFile: index > UINT16_MAX in '" + p.path + "'");
			indices.push_back(static_cast<std::uint16_t>(idx));
		}
		meshDataFilled = false;
		break;
	}
	case MCMeshSource::Kind::Procedural_RandomSpritePoints:
		// Unreachable — handled by the early-return above. Compiler sees this
		// case so the switch is exhaustive over MCMeshSource::Kind.
		break;
	}

	if (meshDataFilled) {
		// GeometryGenerator::Vertex carries Position/Normal/TangentU/TexC. MCVertex
		// is Pos/Normal/TexC. Field-by-field copy; TangentU dropped (Phase 1 has no
		// normal-mapping pipeline that consumes it).
		vertices.resize(meshData.Vertices.size());
		for (size_t i = 0; i < meshData.Vertices.size(); ++i) {
			vertices[i].Pos    = meshData.Vertices[i].Position;
			vertices[i].Normal = meshData.Vertices[i].Normal;
			vertices[i].TexC   = meshData.Vertices[i].TexC;
		}
		indices = meshData.GetIndices16();
	}

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(MCVertex);
	const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MCMeshGeometry>();
	geo->Name = displayName;

	// CPU-side blob copies (kept for parity with the existing scene-side build
	// pattern; debug tools and the d3dUtil examples expect them). Phase 2 may
	// drop these once nothing reads them.
	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	// Upload to default-heap buffers. CreateDefaultBuffer writes the upload-heap
	// staging resource via the open command list and outputs the upload heap
	// through the third arg; we keep it alive on geo so the GPU can finish
	// copying before DisposeUploaders is called (engine init does that after
	// the post-load FlushCommandQueue).
	auto* device  = mEngine.GetDevice();
	auto* cmdList = mEngine.GetCmdList();

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		device, cmdList, indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride     = sizeof(MCVertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;

	// One submesh covering the whole buffer. Procedural sources are
	// single-primitive; the DrawArgs key matches displayName so render-item
	// code can fetch by name if it wants (today: r->SubmeshName == displayName).
	MCSubmeshGeometry submesh;
	submesh.IndexCount         = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	submesh.CreateBounds(vertices);
	geo->DrawArgs[displayName] = submesh;

	return geo;
}

// Procedural_RandomSpritePoints: emits a TreeSpriteVertex {Pos, Size} buffer at
// stride sizeof(TreeSpriteVertex), point-list topology (count points, count
// indices in 0..count-1). Bypasses the MCVertex pipeline because the layout,
// stride, and PrimitiveTopology all differ. v1 source: deprec/Scene_Ch7.cpp:169-214.
//
// Y-position: pulled from regionMin[1] (defaults pin both regionMin[1] and
// regionMax[1] to 8.0 — same as v1's hardcoded y=8). XZ-exclusion zone is a
// square of half-extent exclusionRadius centered on origin (matches v1's
// |x|<20 && |z|<20 push-to-boundary). Random distribution via MathHelper::RandF
// — non-deterministic across boots, accepted; tree positions vary slightly
// run-to-run, which the 8A6 pixel-diff acceptance gate must tolerate.
std::unique_ptr<MCMeshGeometry> MCMeshSourceManager::BuildRandomSpritePoints(
	const std::string& displayName, const MCMeshSource::RandomSpritePointsParams& p)
{
	struct TreeSpriteVertex { DirectX::XMFLOAT3 Pos; DirectX::XMFLOAT2 Size; };

	std::vector<TreeSpriteVertex> vertices(p.count);
	for (int i = 0; i < p.count; ++i) {
		float x = MathHelper::RandF(p.regionMin[0], p.regionMax[0]);
		float z = MathHelper::RandF(p.regionMin[2], p.regionMax[2]);
		if (std::abs(x) < p.exclusionRadius && std::abs(z) < p.exclusionRadius) {
			x = p.exclusionRadius * x / std::abs(x);
			z = p.exclusionRadius * z / std::abs(z);
		}
		vertices[i].Pos  = { x, p.regionMin[1], z };
		vertices[i].Size = { p.spriteSize[0], p.spriteSize[1] };
	}

	std::vector<std::uint16_t> indices(p.count);
	std::iota(indices.begin(), indices.end(), std::uint16_t{ 0 });

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(TreeSpriteVertex);
	const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MCMeshGeometry>();
	geo->Name = displayName;

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	auto* device  = mEngine.GetDevice();
	auto* cmdList = mEngine.GetCmdList();

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(
		device, cmdList, vertices.data(), vbByteSize, geo->VertexBufferUploader);
	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(
		device, cmdList, indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride     = sizeof(TreeSpriteVertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize  = ibByteSize;

	MCSubmeshGeometry submesh;
	submesh.IndexCount         = (UINT)indices.size();
	submesh.StartIndexLocation = 0;
	submesh.BaseVertexLocation = 0;
	// CreateBounds expects MCVertex; sprite-points have a different layout.
	// Bounds are not meaningful for billboard point clouds anyway (the GS
	// expands each point into a 20×20 quad at draw time), so leave the
	// submesh's BoundingBox at its default-constructed zero state.
	geo->DrawArgs[displayName] = submesh;

	return geo;
}

MCMeshSource* MCMeshSourceManager::GetSource(MeshHandle h)
{
	auto it = mSources.find(h);
	return it == mSources.end() ? nullptr : &it->second;
}

MCMeshGeometry* MCMeshSourceManager::GetGeometry(MeshHandle h)
{
	auto* src = GetSource(h);
	return src ? src->geometry.get() : nullptr;
}

bool MCMeshSourceManager::IsLoaded(MeshHandle h) const
{
	return mSources.count(h) > 0;
}

MeshHandle MCMeshSourceManager::Find(const std::string& displayName) const
{
	auto h = HashAssetIdentity<AssetKind::MeshSource>(displayName);
	return mSources.count(h) ? h : 0;
}

void MCMeshSourceManager::RegisterUsedBy(MeshHandle h, const std::string& sceneName)
{
	mUsedBy[h].insert(sceneName);
}

void MCMeshSourceManager::UnregisterUsedBy(MeshHandle h, const std::string& sceneName)
{
	auto it = mUsedBy.find(h);
	if (it != mUsedBy.end()) it->second.erase(sceneName);

	if (it->second.empty()) {
		mUsedBy.erase(it);
		mSources.erase(h);
	}
}

const std::set<std::string>& MCMeshSourceManager::ScenesUsing(MeshHandle h) const
{
	auto it = mUsedBy.find(h);
	return it == mUsedBy.end() ? kEmptyScenes : it->second;
}

// D8 Step 9c — Rename a registered mesh source.
// Under v2 (drop-merging) every MCMeshGeometry has its single DrawArgs entry keyed
// by Geo->Name, so renaming requires rekeying both `geometry->Name` AND the lone
// `DrawArgs[oldDisplay]` entry — unconditionally, no kind filter. No on-disk file
// to rename: procedural mesh data lives inline in scene JSON's meshes array, and
// the asset redirect (RecordAssetRedirect below) covers the handle change for
// existing scene loads.
MeshHandle MCMeshSourceManager::Rename(MeshHandle oldHandle, const std::string& newDisplayName) {
	auto it = mSources.find(oldHandle);
	if (it == mSources.end())
		throw std::runtime_error("MCMeshSourceManager::Rename: handle not loaded");
	auto newHandle = HashAssetIdentity<AssetKind::MeshSource>(newDisplayName);
	if (mSources.count(newHandle))
		throw std::runtime_error("MCMeshSourceManager::Rename: target name '" + newDisplayName + "' already registered");

	MCMeshSource src = std::move(it->second);

	// Every kind has DrawArgs keyed by Geo->Name (drop-merging invariant). Rekey
	// unconditionally. Capture old name BEFORE mutating geometry->Name.
	if (src.geometry) {
		const std::string oldDisplay = src.geometry->Name;
		if (auto daIt = src.geometry->DrawArgs.find(oldDisplay); daIt != src.geometry->DrawArgs.end()) {
			auto submesh = std::move(daIt->second);
			src.geometry->DrawArgs.erase(daIt);
			src.geometry->DrawArgs[newDisplayName] = std::move(submesh);
		}
		src.geometry->Name = newDisplayName;
	}

	mSources.emplace(newHandle, std::move(src));   // insert before erase — exception safety, mirrors 9b
	mSources.erase(oldHandle);                     // erase by KEY, not iterator: emplace above may have rehashed

	if (auto uIt = mUsedBy.find(oldHandle); uIt != mUsedBy.end()) {
		mUsedBy[newHandle] = std::move(uIt->second);
		mUsedBy.erase(uIt);
	}

	mEngine.Scenes().RecordAssetRedirect("mesh", oldHandle, newHandle, "Rename via API");
	return newHandle;
}
