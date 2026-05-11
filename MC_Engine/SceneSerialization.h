#pragma once

#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include "RenderItem.h"

// Math primitives — ADL pairs live in DirectX::
namespace DirectX {
    void to_json(nlohmann::json& j, const XMFLOAT2& v);
    void from_json(const nlohmann::json& j, XMFLOAT2& v);
    void to_json(nlohmann::json& j, const XMFLOAT3& v);
    void from_json(const nlohmann::json& j, XMFLOAT3& v);
    void to_json(nlohmann::json& j, const XMFLOAT4& v);
    void from_json(const nlohmann::json& j, XMFLOAT4& v);
    void to_json(nlohmann::json& j, const XMFLOAT4X4& m);
    void from_json(const nlohmann::json& j, XMFLOAT4X4& m);
}
void to_json(nlohmann::json& j, const RenderItem& r);
void from_json(const nlohmann::json& j, RenderItem& r);
// this must match RenderLayer in RenderItem.h
NLOHMANN_JSON_SERIALIZE_ENUM(RenderLayer, {
    {RenderLayer::Opaque, "Opaque"},
    {RenderLayer::Mirrors, "Mirrors"},
    {RenderLayer::Reflected, "Reflected"},
    {RenderLayer::Transparent, "Transparent"},
    {RenderLayer::AlphaTested, "AlphaTested"},
    {RenderLayer::Shadow, "Shadow"},
    {RenderLayer::AlphaTestedTreeSprites, "AlphaTestedTreeSprites"},
    {RenderLayer::OpaqueTessellated, "OpaqueTessellated"},
    {RenderLayer::OpaqueInstanced, "OpaqueInstanced"},
    {RenderLayer::GrassInstanced, "GrassInstanced"},
    {RenderLayer::AlphaTestedInstanced, "AlphaTestedInstanced"},
});

void RunJsonRoundTripTests();