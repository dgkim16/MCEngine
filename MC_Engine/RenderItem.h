#pragma once
#include "d3dUtil.h"
#include "MathHelper.h"
#include "FrameResource.h"
#include "../MCMaterial.h"
#include "../MCMeshGeometry.h"
#include "MCAssetIdentity.h"
extern const int gNumFrameResources;

struct InstanceCell {
	DirectX::BoundingBox WorldBounds;  // world-space AABB covering all instances in this cell
	int StartIndex = 0;                // index into Instances[] for the first member
	int Count = 0;					   // number of contiguous instances in this cell
};

struct RenderItem
{
	RenderItem() = default;

	int NumFramesDirty = gNumFrameResources;
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4(); // cache
	DirectX::XMFLOAT3 Position { 0,0,0 }; // Serialized
	DirectX::XMFLOAT4 Rotation { 0, 0, 0, 1 }; // Serialized (Quaternion)
	DirectX::XMFLOAT3 Scale { 1,1,1 };	// Serialized
	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4(); // Serialized

	std::string Name = "NA"; // Serialized
	uint32_t    Layers = 0;	 // Serialized - bitmask of RenderLayer
	UINT ObjCBIndex = -1;
	UINT ObjInstIndex = -1;

	MaterialHandle materialHandle = 0;
	MeshHandle     meshHandle = 0;

	MCMaterial* Mat = nullptr;
	MCMeshGeometry* Geo = nullptr;
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	std::string SubmeshName;

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

// this must match SceneSerialization.h NLOHMANN_JSON_SERIALIZE_ENUM
// enum value is the bit index, not the bit value!
// Values are ordinals; bit position via LayerBit(l). 
// Used as index into mRitemLayer[Count] and as bit-shift source for RenderItem::Layers.
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

static_assert(static_cast<int>(RenderLayer::Count) <= 32,
	"RenderLayer count exceeds uint32_t bitmask capacity. "
	"Switch to uint64_t or a different representation.");

inline uint32_t LayerBit(RenderLayer l) {
	return 1u << static_cast<uint32_t>(l);
}

inline bool HasLayer(uint32_t mask, RenderLayer l) {
	return (mask & LayerBit(l)) != 0;
}

inline const char* RenderLayerName(RenderLayer layer) {
	switch (layer) {
	case RenderLayer::Opaque:                 return "Opaque";
	case RenderLayer::Mirrors:                return "Mirrors";
	case RenderLayer::Reflected:              return "Reflected";
	case RenderLayer::Transparent:            return "Transparent";
	case RenderLayer::AlphaTested:            return "AlphaTested";
	case RenderLayer::Shadow:                 return "Shadow";
	case RenderLayer::AlphaTestedTreeSprites: return "AlphaTestedTreeSprites";
	case RenderLayer::OpaqueTessellated:      return "OpaqueTessellated";
	case RenderLayer::OpaqueInstanced:        return "OpaqueInstanced";
	case RenderLayer::GrassInstanced:         return "GrassInstanced";
	case RenderLayer::AlphaTestedInstanced:   return "AlphaTestedInstanced";
	default:                                  return "<Unknown>";
	}
}

// LayerMask convention :
//   0 bits  -> "Nothing"
//   1 bit   -> the layer name
//   N bits  -> "Mixed..."
//   all     -> "Everything"
static const char* LayerMaskSummary(uint32_t mask) {
	constexpr uint32_t kAll =
		(1u << static_cast<uint32_t>(RenderLayer::Count)) - 1u;
	if (mask == 0)    return "Nothing";
	if (mask == kAll) return "Everything";
	if (std::popcount(mask) == 1) {                    // C++20; or __popcnt(mask)
		for (uint32_t i = 0; i < static_cast<uint32_t>(RenderLayer::Count); ++i)
			if (mask & (1u << i))
				return RenderLayerName(static_cast<RenderLayer>(i));
	}
	return "Mixed...";
}