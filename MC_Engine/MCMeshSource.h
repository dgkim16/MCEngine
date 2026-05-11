#pragma once
#include "MCAssetIdentity.h"
#include "MCMeshGeometry.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <array>
#include <nlohmann/json.hpp>

// Asset-side mesh source. Owns its MCMeshGeometry GPU realization. The on-disk
// representation is inline in scene JSON (Day 8); there is no .mcmesh file.
//
// Every mesh in the engine is expressed as a kind + per-kind named params.
// The v1 "External" classification (hand-coded merged buffers) is gone with
// the drop-merging design (Step 8-After) — every kind is built by the
// manager from data that lives in scene JSON.
struct MCMeshSource {
	enum class Kind {
		Procedural_Box,
		Procedural_Sphere,
		Procedural_Cylinder,
		Procedural_Grid,
		Procedural_GeoSphere,
		Procedural_Quad,
		Procedural_GrassPatch,
		Procedural_RandomSpritePoints,
		ImportedFromFile,
	};

	// Per-kind named parameter structs. Defaults match v1 startup state so a
	// default-constructed source produces a sane mesh without explicit params.
	struct BoxParams       { float w = 1.0f, h = 1.0f, d = 1.0f; int subdivisions = 0; };
	struct SphereParams    { float radius = 0.5f; int slices = 20, stacks = 20; };
	struct CylinderParams  { float bottomRadius = 0.5f, topRadius = 0.3f, height = 3.0f; int slices = 20, stacks = 20; };
	struct GridParams      { float w = 1.0f, d = 1.0f; int m = 2, n = 2; };
	struct GeoSphereParams { float radius = 0.5f; int subdivisions = 2; };
	struct QuadParams      { float x = -1.0f, y = -1.0f, w = 2.0f, h = 2.0f, z = 0.0f; };
	struct GrassPatchParams { float width = 0.4f, height = 1.0f, sharpness = 0.9f; };
	struct ImportedFromFileParams { std::string path; };
	struct RandomSpritePointsParams {
		int count = 16;
		std::array<float, 3> regionMin = { -45.0f, 8.0f, -45.0f };
		std::array<float, 3> regionMax = {  45.0f, 8.0f,  45.0f };
		float exclusionRadius = 20.0f;             // square XZ exclusion zone, half-extent, centered on origin
		std::array<float, 2> spriteSize = { 20.0f, 20.0f };
	};

	using Params = std::variant<
		BoxParams, SphereParams, CylinderParams, GridParams,
		GeoSphereParams, QuadParams, GrassPatchParams,
		ImportedFromFileParams, RandomSpritePointsParams>;

	Kind kind = Kind::Procedural_Box;
	Params params = BoxParams{};

	// GPU realization. Owned by the asset; non-null after a successful Register.
	// Built by MCMeshSourceManager::BuildProcedural at Register time; no caller
	// is expected to pre-build the geometry under the v2 model.
	std::unique_ptr<MCMeshGeometry> geometry;

	// --- JSON helpers (D8 Step 5c, refactored to variant in 8A1) ------------
	// String ↔ Kind mapping for scene JSON's "kind" field. Throws on unknown
	// string so a typo ("Procedral_Box") surfaces with the bad string in the
	// message, not at the next manager call.
	static Kind ParseKind(const std::string& s) {
		if (s == "Procedural_Box")                 return Kind::Procedural_Box;
		if (s == "Procedural_Sphere")              return Kind::Procedural_Sphere;
		if (s == "Procedural_Cylinder")            return Kind::Procedural_Cylinder;
		if (s == "Procedural_Grid")                return Kind::Procedural_Grid;
		if (s == "Procedural_GeoSphere")           return Kind::Procedural_GeoSphere;
		if (s == "Procedural_Quad")                return Kind::Procedural_Quad;
		if (s == "Procedural_GrassPatch")          return Kind::Procedural_GrassPatch;
		if (s == "Procedural_RandomSpritePoints")  return Kind::Procedural_RandomSpritePoints;
		if (s == "ImportedFromFile")               return Kind::ImportedFromFile;
		throw std::runtime_error("MCMeshSource::ParseKind: unknown kind '" + s + "'");
	}

	static const char* KindToString(Kind k) {
		switch (k) {
		case Kind::Procedural_Box:                return "Procedural_Box";
		case Kind::Procedural_Sphere:             return "Procedural_Sphere";
		case Kind::Procedural_Cylinder:           return "Procedural_Cylinder";
		case Kind::Procedural_Grid:               return "Procedural_Grid";
		case Kind::Procedural_GeoSphere:          return "Procedural_GeoSphere";
		case Kind::Procedural_Quad:               return "Procedural_Quad";
		case Kind::Procedural_GrassPatch:         return "Procedural_GrassPatch";
		case Kind::Procedural_RandomSpritePoints: return "Procedural_RandomSpritePoints";
		case Kind::ImportedFromFile:              return "ImportedFromFile";
		}
		throw std::runtime_error("MCMeshSource::KindToString: unhandled kind");
	}

	// Reads the "params" JSON object into src.params via per-kind named fields.
	// Caller has already set src.kind via ParseKind. Missing fields fall back
	// to in-class defaults so a partial JSON entry doesn't throw — only an
	// outright kind mismatch does.
	static void ParseParams(MCMeshSource& src, const nlohmann::json& p) {
		switch (src.kind) {
		case Kind::Procedural_Box: {
			BoxParams x;
			x.w            = p.value("w", x.w);
			x.h            = p.value("h", x.h);
			x.d            = p.value("d", x.d);
			x.subdivisions = p.value("subdivisions", x.subdivisions);
			src.params = x;
			break;
		}
		case Kind::Procedural_Sphere: {
			SphereParams x;
			x.radius = p.value("radius", x.radius);
			x.slices = p.value("slices", x.slices);
			x.stacks = p.value("stacks", x.stacks);
			src.params = x;
			break;
		}
		case Kind::Procedural_Cylinder: {
			CylinderParams x;
			x.bottomRadius = p.value("bottomRadius", x.bottomRadius);
			x.topRadius    = p.value("topRadius",    x.topRadius);
			x.height       = p.value("height",       x.height);
			x.slices       = p.value("slices",       x.slices);
			x.stacks       = p.value("stacks",       x.stacks);
			src.params = x;
			break;
		}
		case Kind::Procedural_Grid: {
			GridParams x;
			x.w = p.value("w", x.w);
			x.d = p.value("d", x.d);
			x.m = p.value("m", x.m);
			x.n = p.value("n", x.n);
			src.params = x;
			break;
		}
		case Kind::Procedural_GeoSphere: {
			GeoSphereParams x;
			x.radius       = p.value("radius",       x.radius);
			x.subdivisions = p.value("subdivisions", x.subdivisions);
			src.params = x;
			break;
		}
		case Kind::Procedural_Quad: {
			QuadParams q;
			q.x = p.value("x", q.x);
			q.y = p.value("y", q.y);
			q.w = p.value("w", q.w);
			q.h = p.value("h", q.h);
			q.z = p.value("z", q.z);
			src.params = q;
			break;
		}
		case Kind::Procedural_GrassPatch: {
			GrassPatchParams x;
			x.width     = p.value("width",     x.width);
			x.height    = p.value("height",    x.height);
			x.sharpness = p.value("sharpness", x.sharpness);
			src.params = x;
			break;
		}
		case Kind::Procedural_RandomSpritePoints: {
			RandomSpritePointsParams x;
			x.count           = p.value("count",           x.count);
			if (auto it = p.find("regionMin");  it != p.end()) x.regionMin  = it->get<std::array<float, 3>>();
			if (auto it = p.find("regionMax");  it != p.end()) x.regionMax  = it->get<std::array<float, 3>>();
			x.exclusionRadius = p.value("exclusionRadius", x.exclusionRadius);
			if (auto it = p.find("spriteSize"); it != p.end()) x.spriteSize = it->get<std::array<float, 2>>();
			src.params = x;
			break;
		}
		case Kind::ImportedFromFile: {
			ImportedFromFileParams x;
			x.path = p.value("path", x.path);
			src.params = x;
			break;
		}
		}
	}

	// Inverse of ParseParams: emits the per-kind named-fields object that
	// ToJson writes under the "params" key. Mirror of the cases above so a
	// SaveActive → LoadFromJson round-trip is symmetric.
	static nlohmann::json ParamsToJson(const Params& v) {
		nlohmann::json j;
		std::visit([&](const auto& x) {
			using T = std::decay_t<decltype(x)>;
			if constexpr (std::is_same_v<T, BoxParams>) {
				j["w"] = x.w; j["h"] = x.h; j["d"] = x.d; j["subdivisions"] = x.subdivisions;
			} else if constexpr (std::is_same_v<T, SphereParams>) {
				j["radius"] = x.radius; j["slices"] = x.slices; j["stacks"] = x.stacks;
			} else if constexpr (std::is_same_v<T, CylinderParams>) {
				j["bottomRadius"] = x.bottomRadius; j["topRadius"] = x.topRadius;
				j["height"] = x.height; j["slices"] = x.slices; j["stacks"] = x.stacks;
			} else if constexpr (std::is_same_v<T, GridParams>) {
				j["w"] = x.w; j["d"] = x.d; j["m"] = x.m; j["n"] = x.n;
			} else if constexpr (std::is_same_v<T, GeoSphereParams>) {
				j["radius"] = x.radius; j["subdivisions"] = x.subdivisions;
			} else if constexpr (std::is_same_v<T, QuadParams>) {
				j["x"] = x.x; j["y"] = x.y; j["w"] = x.w; j["h"] = x.h; j["z"] = x.z;
			} else if constexpr (std::is_same_v<T, GrassPatchParams>) {
				j["width"] = x.width; j["height"] = x.height; j["sharpness"] = x.sharpness;
			} else if constexpr (std::is_same_v<T, RandomSpritePointsParams>) {
				j["count"] = x.count;
				j["regionMin"] = x.regionMin;
				j["regionMax"] = x.regionMax;
				j["exclusionRadius"] = x.exclusionRadius;
				j["spriteSize"] = x.spriteSize;
			} else if constexpr (std::is_same_v<T, ImportedFromFileParams>) {
				j["path"] = x.path;
			}
		}, v);
		return j;
	}
};
