#pragma once
#include "d3dUtil.h"
#include "MathHelper.h"
#include "FrameResource.h"
extern const int gNumFrameResources;

struct InstanceCell {
	DirectX::BoundingBox WorldBounds;  // world-space AABB covering all instances in this cell
	int StartIndex = 0;                // index into Instances[] for the first member
	int Count = 0;					   // number of contiguous instances in this cell
};

struct RenderItem
{
	RenderItem() = default;

	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	int NumFramesDirty = gNumFrameResources;

	std::string Name = "NA";
	UINT ObjCBIndex = -1;
	UINT ObjInstIndex = -1;
	MeshGeometry* Geo = nullptr;
	Material* Mat = nullptr;
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	UINT IndexCount = 0;
	UINT StartIndexLocation = 0;
	int BaseVertexLocation = 0;

	std::vector<InstanceData> Instances; // larger InstanceData
	UINT InstanceCount = 0;
	UINT InstanceBufferStartIndex = 0;

	std::vector<GrassInstanceData> GrassInstances; // smaller InstanceData 
	UINT GrassInstanceBufferStartIndex = 0;

	DirectX::BoundingBox Bounds;
	std::vector<InstanceCell> InstanceCells;   // populated by scene; empty = cell culling unused
	bool useCellCulling = false;               // toggle: cell-AABB vs per-instance frustum test
	int bbInstTestCount = 0;

	bool checkBounds = true;
	bool insideFrustrum = true;
};

enum class RenderLayer : int
{
	Opaque = 0,
	Mirrors,
	Reflected,
	Transparent,
	AlphaTested,
	Shadow,
	AlphaTestedTreeSprites,
	OpaqueTessellated,
	OpaqueInstanced,
	GrassInstanced,
	AlphaTestedInstanced,
	Count
};
