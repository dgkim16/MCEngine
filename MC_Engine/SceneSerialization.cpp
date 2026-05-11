#include "SceneSerialization.h"

namespace DirectX {
    void to_json(nlohmann::json& j, const XMFLOAT2& v) {
        j = nlohmann::json{ {"x", v.x}, {"y", v.y} };
    }
    void from_json(const nlohmann::json& j, XMFLOAT2& v) {
        j.at("x").get_to(v.x);
        j.at("y").get_to(v.y);
    }
    void to_json(nlohmann::json& j, const XMFLOAT3& v) {
        j = nlohmann::json{ {"x", v.x}, {"y", v.y}, {"z", v.z} };
    }
    void from_json(const nlohmann::json& j, XMFLOAT3& v) {
        j.at("x").get_to(v.x);
        j.at("y").get_to(v.y);
        j.at("z").get_to(v.z);
    }
    void to_json(nlohmann::json& j, const XMFLOAT4& v) {
        j = nlohmann::json{ {"x", v.x}, {"y", v.y}, {"z", v.z}, {"w", v.w} };
    }
    void from_json(const nlohmann::json& j, XMFLOAT4& v) {
        j.at("x").get_to(v.x);
        j.at("y").get_to(v.y);
        j.at("z").get_to(v.z);
        j.at("w").get_to(v.w);
    }
    void to_json(nlohmann::json& j, const XMFLOAT4X4& m) {
        j = nlohmann::json{
            {"m00", m._11}, {"m01", m._12}, {"m02", m._13}, {"m03", m._14},
            {"m10", m._21}, {"m11", m._22}, {"m12", m._23}, {"m13", m._24},
            {"m20", m._31}, {"m21", m._32}, {"m22", m._33}, {"m23", m._34},
            {"m30", m._41}, {"m31", m._42}, {"m32", m._43}, {"m33", m._44},
        };
    }
    void from_json(const nlohmann::json& j, XMFLOAT4X4& m) {
        j.at("m00").get_to(m._11); j.at("m01").get_to(m._12); j.at("m02").get_to(m._13); j.at("m03").get_to(m._14);
        j.at("m10").get_to(m._21); j.at("m11").get_to(m._22); j.at("m12").get_to(m._23); j.at("m13").get_to(m._24);
        j.at("m20").get_to(m._31); j.at("m21").get_to(m._32); j.at("m22").get_to(m._33); j.at("m23").get_to(m._34);
        j.at("m30").get_to(m._41); j.at("m31").get_to(m._42); j.at("m32").get_to(m._43); j.at("m33").get_to(m._44);
    }
    
}

// RenderItem ADL pair lives in the global namespace because RenderItem is declared in
// the global namespace (see RenderItem.h). Putting these inside namespace DirectX would
// hide them from ADL on RenderItem, breaking j.get_to(*r) / j.get<RenderItem>().
void to_json(nlohmann::json& j, const RenderItem& r) {
    j = nlohmann::json{
        {"Position",      r.Position},                   // XMFLOAT3
        {"Rotation",      r.Rotation},                   // XMFLOAT4 quaternion (x,y,z,w)
        {"Scale",         r.Scale},                      // XMFLOAT3
        {"TexTransform",  r.TexTransform},               // XMFLOAT4X4
        {"Name",          r.Name},
        {"PrimitiveType", static_cast<int>(r.PrimitiveType)},
        {"useCellCulling", r.useCellCulling},
        {"checkBounds",   r.checkBounds},
        // SubmeshName dropped in 8A3: under drop-merging every MCMeshGeometry
        // has exactly one DrawArgs entry keyed by Geo->Name, so the render-item
        // mesh reference is single-level (meshHandle alone). The DrawArgs lookup
        // at draw time uses Geo->Name; r.SubmeshName is no longer authoring data.
    };
    // Layers bitmask -> array of layer name strings.
    // Bit-walk in declaration order for stable on-disk ordering across diffs.
    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t i = 0; i < static_cast<uint32_t>(RenderLayer::Count); ++i) {
        if (r.Layers & (1u << i)) {
            arr.push_back(static_cast<RenderLayer>(i));  // SERIALIZE_ENUM emits string
        }
    }
    j["layers"] = std::move(arr);
    j["materialHandle"] = std::to_string(r.materialHandle);
    j["meshHandle"] = std::to_string(r.meshHandle);
}

// called by MCEngine::LoadRenderItemFromJson() - j.get_to(*r);
void from_json(const nlohmann::json& j, RenderItem& r) {
    j.at("Position").get_to(r.Position);
    j.at("Rotation").get_to(r.Rotation);
    j.at("Scale").get_to(r.Scale);
    j.at("TexTransform").get_to(r.TexTransform);
    j.at("Name").get_to(r.Name);
    r.materialHandle = std::stoull(j.at("materialHandle").get<std::string>());
    r.meshHandle = std::stoull(j.at("meshHandle").get<std::string>());
    int primInt = j.at("PrimitiveType").get<int>();
    r.PrimitiveType = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(primInt);
    // SubmeshName not read — see to_json comment. LoadRenderItemsFromJson sets
    // r.SubmeshName = r.Geo->Name after the mesh handle resolves, so anything
    // that still reads r.SubmeshName at draw time finds a sensible value.
    j.at("useCellCulling").get_to(r.useCellCulling);
    j.at("checkBounds").get_to(r.checkBounds);

    // Array of layer name strings -> bitmask.
    r.Layers = 0;
    for (const auto& el : j.at("layers")) {
        RenderLayer l = el.get<RenderLayer>();   // SERIALIZE_ENUM parses string
        r.Layers |= LayerBit(l);
    }
    if (r.Layers == 0) {
        throw std::runtime_error(
            "MCScene file: RenderItem '" + r.Name + "' has empty layers array");
    }
}

#define MC_ASSERT(cond, name) assert((cond) && (name))

void RunJsonRoundTripTests() {
    using namespace DirectX;
    using nlohmann::json;

    // ---- Identity round-trips ----

    // XMFLOAT2
    {
        XMFLOAT2 a{ 1.0f, -2.5f };
        XMFLOAT2 b = json(a).get<XMFLOAT2>();
        MC_ASSERT(b.x == a.x && b.y == a.y, "XMFLOAT2 round-trip");
    }
    // XMFLOAT3
    {
        XMFLOAT3 a{ 1.0f, -2.5f, 3.14159f };
        XMFLOAT3 b = json(a).get<XMFLOAT3>();
        MC_ASSERT(b.x == a.x && b.y == a.y && b.z == a.z, "XMFLOAT3 round-trip");
    }
    // XMFLOAT4
    {
        XMFLOAT4 a{ 1.0f, -2.5f, 3.14159f, 0.0f };
        XMFLOAT4 b = json(a).get<XMFLOAT4>();
        MC_ASSERT(b.x == a.x && b.y == a.y && b.z == a.z && b.w == a.w,
            "XMFLOAT4 round-trip");
    }
    // XMFLOAT4X4 — use distinct values per cell so a transposed
    // or off-by-one mapping would actually fail the assert.
    {
        XMFLOAT4X4 a(
            1.0f, 2.0f, 3.0f, 4.0f,
            5.0f, 6.0f, 7.0f, 8.0f,
            9.0f, 10.0f, 11.0f, 12.0f,
            13.0f, 14.0f, 15.0f, 16.0f);
        XMFLOAT4X4 b = json(a).get<XMFLOAT4X4>();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                MC_ASSERT(a.m[r][c] == b.m[r][c], "XMFLOAT4X4 cell round-trip");
    }

    // ---- Render layer enum ----
    {
        RenderLayer a = RenderLayer::AlphaTestedTreeSprites;
        json j = a;
        MC_ASSERT(j.get<std::string>() == "AlphaTestedTreeSprites",
            "RenderLayer to string");
        RenderLayer b = j.get<RenderLayer>();
        MC_ASSERT(b == a, "RenderLayer round-trip");
    }
    // Every enum value round-trips — catches a missing entry in the
    // NLOHMANN_JSON_SERIALIZE_ENUM table.
    {
        for (int i = 0; i < static_cast<int>(RenderLayer::Count); ++i) {
            RenderLayer a = static_cast<RenderLayer>(i);
            RenderLayer b = json(a).get<RenderLayer>();
            MC_ASSERT(a == b, "RenderLayer all values round-trip");
        }
    }

    // ---- Hostile cases ----

    // Unknown layer string falls back to default (first entry).
    {
        json j = "MadeUpLayerName";
        RenderLayer b = j.get<RenderLayer>();
        MC_ASSERT(b == RenderLayer::Opaque,
            "Unknown RenderLayer falls back to default");
    }
    // Missing field throws on read.
    {
        json j = json::parse(R"({"x": 1.0, "y": 2.0})");  // no "z"
        bool threw = false;
        try { (void)j.get<XMFLOAT3>(); }
        catch (const json::out_of_range&) { threw = true; }
        MC_ASSERT(threw, "Missing field throws out_of_range");
    }
    // Wrong field type throws on read.
    {
        json j = json::parse(R"({"x": 1.0, "y": "not a number", "z": 3.0})");
        bool threw = false;
        try { (void)j.get<XMFLOAT3>(); }
        catch (const json::type_error&) { threw = true; }
        MC_ASSERT(threw, "Wrong field type throws type_error");
    }
    // Matrix missing a single cell throws on read.
    {
        json j = json(XMFLOAT4X4(
            1, 2, 3, 4, 5, 6, 7, 8,
            9, 10, 11, 12, 13, 14, 15, 16));
        j.erase("m23");
        bool threw = false;
        try { (void)j.get<XMFLOAT4X4>(); }
        catch (const json::out_of_range&) { threw = true; }
        MC_ASSERT(threw, "Matrix missing cell throws out_of_range");
    }
    // String form survives a parse → serialize cycle byte-identically.
    // Catches accidental key reordering or precision loss in to_json.
    {
        XMFLOAT3 a{ 1.5f, -2.25f, 0.125f };  // exact in float
        std::string s1 = json(a).dump();
        XMFLOAT3 b = json::parse(s1).get<XMFLOAT3>();
        std::string s2 = json(b).dump();
        MC_ASSERT(s1 == s2, "Dump-parse-dump is byte-stable");
    }

    OutputDebugStringA("[json] round-trip tests PASSED\n");
}