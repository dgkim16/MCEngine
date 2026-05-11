#include "AuthorScenes.h"

#ifdef MC_AUTHOR_SCENES_ONCE

#include "MCMeshSource.h"
#include "MCAssetIdentity.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

// One-shot author tool. The implementation reads v1-shape scene_*.json files,
// transforms each into the v2 eight-key shape, and writes them back. Each
// scene's recipe (which procedural meshes the scene needs, which special
// pointers to populate, which modules to attach) is hand-coded below — that
// is what "scene-specific authoring data" means. Once committed, this file
// becomes dead weight and should be removed from the build.

namespace AuthorScenes {

namespace {

// One mesh entry's recipe: the v2 displayName, the v1 SubmeshName it replaces
// in render-item meshHandle resolution, and the kind+params that BuildProcedural
// will read at runtime.
struct MeshRecipe {
	std::string newDisplayName;
	std::string v1SubmeshName;
	MCMeshSource::Kind kind;
	nlohmann::json paramsJson;
};

// Translate v1's "merged buffer + SubmeshName" render-item shape into v2's
// "single mesh handle" shape, then assemble the eight-key v2 JSON.
void EmitScene(const std::string& sceneName,
               const std::string& v1Path,
               const std::string& v2Path,
               const std::vector<MeshRecipe>& recipes,
               const nlohmann::json& specialPointers,
               const nlohmann::json& modules)
{
	// Load v1 JSON. Empty scene's v1Path may be empty — start with an empty
	// renderItems array.
	nlohmann::json v1;
	if (!v1Path.empty()) {
		std::ifstream in(v1Path);
		if (!in) throw std::runtime_error("AuthorScenes: cannot read '" + v1Path + "'");
		in >> v1;
	}
	if (!v1.contains("renderItems")) v1["renderItems"] = nlohmann::json::array();

	auto findRecipe = [&](const std::string& sub) -> const MeshRecipe* {
		for (const auto& r : recipes) if (r.v1SubmeshName == sub) return &r;
		return nullptr;
	};

	nlohmann::json v2;
	v2["version"]   = 1;
	v2["sceneName"] = sceneName;

	// meshes[]: each entry is {name, kind, params:{...}}.
	v2["meshes"] = nlohmann::json::array();
	for (const auto& r : recipes) {
		nlohmann::json m;
		m["name"]   = r.newDisplayName;
		m["kind"]   = MCMeshSource::KindToString(r.kind);
		m["params"] = r.paramsJson;
		v2["meshes"].push_back(std::move(m));
	}

	// renderItems[]: copy v1, replace meshHandle, drop SubmeshName.
	std::set<std::uint64_t> mats;
	v2["renderItems"] = nlohmann::json::array();
	for (const auto& v1Item : v1["renderItems"]) {
		nlohmann::json v2Item = v1Item;

		const std::string sub = v1Item.value("SubmeshName", std::string{});
		const MeshRecipe* recipe = findRecipe(sub);
		if (!recipe) {
			throw std::runtime_error(
				"AuthorScenes: scene '" + sceneName + "' render item '" +
				v1Item.value("Name", std::string{}) + "' has unknown SubmeshName '" +
				sub + "' (no recipe maps it to a v2 mesh)");
		}

		const auto newHandle = HashAssetIdentity<AssetKind::MeshSource>(recipe->newDisplayName);
		v2Item["meshHandle"] = std::to_string(newHandle);
		v2Item.erase("SubmeshName");
		v2["renderItems"].push_back(std::move(v2Item));

		// Aggregate materialHandles for the scene-side handle list.
		mats.insert(std::stoull(v1Item.at("materialHandle").get<std::string>()));
	}

	// materialHandles[]: aggregated unique set from render items.
	v2["materialHandles"] = nlohmann::json::array();
	for (auto h : mats) v2["materialHandles"].push_back(std::to_string(h));

	// textureHandles[]: empty for now. Render items resolve their own materials
	// at LoadRenderItemsFromJson time, and materials know their textures
	// internally. Scene-side textureHandles is purely a usage-tracking list;
	// leaving it empty produces incomplete usage tracking but does not affect
	// rendering. Phase 2 polish: derive the union from the materials.
	v2["textureHandles"] = nlohmann::json::array();

	// specialPointers + modules: hand-coded per scene by the caller.
	v2["specialPointers"] = specialPointers;
	v2["modules"]         = modules;

	// Atomic write: tmp + rename. Same pattern MCSceneManager::RecordAssetRedirect
	// uses for asset_redirects.json — partial writes that crash mid-author would
	// otherwise wedge the next boot.
	const std::string tmpPath = v2Path + ".tmp";
	{
		std::ofstream out(tmpPath);
		if (!out) throw std::runtime_error("AuthorScenes: cannot write '" + tmpPath + "'");
		out << v2.dump(2);
	}
	std::filesystem::rename(tmpPath, v2Path);
	std::cout << "[AuthorScenes] wrote " << v2Path << "\n";
}

} // anonymous namespace

void EmitAll() {
	using K = MCMeshSource::Kind;

	// ============ Ch7 ============
	// Seven meshes, derived from Scene_Ch7::BuildGeometry / BuildSpriteGeometry
	// (deprec/Scene_Ch7.cpp). Names use a "ch7_*_mesh" convention to keep them
	// distinct from same-named render items (different AssetKind hash, but the
	// suffix avoids visual confusion in JSON).
	const std::vector<MeshRecipe> ch7 = {
		{"ch7_box_mesh",         "box",      K::Procedural_Box,
			{{"w", 1.5}, {"h", 0.5}, {"d", 1.5}, {"subdivisions", 3}}},
		{"ch7_grid_mesh",        "grid",     K::Procedural_Grid,
			{{"w", 20.0}, {"d", 30.0}, {"m", 60}, {"n", 40}}},
		{"ch7_sphere_mesh",      "sphere",   K::Procedural_Sphere,
			{{"radius", 0.5}, {"slices", 20}, {"stacks", 20}}},
		{"ch7_cylinder_mesh",    "cylinder", K::Procedural_Cylinder,
			{{"bottomRadius", 0.5}, {"topRadius", 0.3}, {"height", 3.0}, {"slices", 20}, {"stacks", 20}}},
		{"ch7_model_mesh",       "model",    K::ImportedFromFile,
			{{"path", "Assets/Models/다람디.obj"}}},
		{"ch7_quad_mesh",        "quad",     K::Procedural_Quad,
			{{"x", -1.0}, {"y", -1.0}, {"w", 2.0}, {"h", 2.0}, {"z", 0.0}}},
		{"ch7_treesprites_mesh", "points",   K::Procedural_RandomSpritePoints,
			{{"count", 16},
			 {"regionMin", nlohmann::json::array({-45.0, 8.0, -45.0})},
			 {"regionMax", nlohmann::json::array({ 45.0, 8.0,  45.0})},
			 {"exclusionRadius", 20.0},
			 {"spriteSize", nlohmann::json::array({20.0, 20.0})}}},
	};
	const nlohmann::json ch7SpecialPointers = {
		{"modelRitem",          "ch7_model"},
		{"reflectedModelRitem", "ch7_model_reflected"},
		{"tessellatedRitem",    "ch7_quad"},
	};
	EmitScene("Ch7",
	          "Assets/scenes/scene_ch7.json",
	          "Assets/scenes/scene_ch7.json",
	          ch7,
	          ch7SpecialPointers,
	          nlohmann::json::array());

	// ============ Grass ============
	// Two meshes (ground plane + grass-blade quad) plus the MCGrassCullingModule.
	// Module params match MCGrassCullingModule's in-class defaults; if the user
	// later edits the sliders and re-saves, those values overwrite this set.
	const std::vector<MeshRecipe> grass = {
		{"grass_grid_mesh",  "grid", K::Procedural_Grid,
			{{"w", 500.0}, {"d", 500.0}, {"m", 2}, {"n", 2}}},
		{"grass_blade_mesh", "quad", K::Procedural_GrassPatch,
			{{"width", 0.4}, {"height", 1.0}, {"sharpness", 0.9}}},
	};
	nlohmann::json grassModules = nlohmann::json::array();
	{
		nlohmann::json m;
		m["type"] = "MCGrassCulling";
		m["params"] = {
			{"grassWidth",         0.4},
			{"grassHeight",        1.0},
			{"grassCountWidth",    500},
			{"grassCountDepth",    500},
			{"grassCoverageWidth", 500.0},
			{"grassCoverageDepth", 500.0},
			{"grassSharpness",     0.9},
			{"grassRitemName",     "quad"},   // render-item Name in scene_grass.json
			{"planeRitemName",     "grid"},
			{"numCellsX",          5},
			{"numCellsZ",          5},
			{"useGpuCulling",      false},
			{"equalColor",         true},
		};
		grassModules.push_back(std::move(m));
	}
	EmitScene("Grass",
	          "Assets/scenes/scene_grass.json",
	          "Assets/scenes/scene_grass.json",
	          grass,
	          nlohmann::json::object(),
	          grassModules);

	// ============ Empty ============
	// No meshes, no render items, no modules. The scene exists so the engine has
	// a "switch to clear-color view" target for debugging.
	EmitScene("Empty",
	          "",                                  // no v1 input
	          "Assets/scenes/scene_empty.json",
	          {},
	          nlohmann::json::object(),
	          nlohmann::json::array());

	std::cout << "[AuthorScenes] all scenes written. Remove -DMC_AUTHOR_SCENES_ONCE and rebuild for normal boot.\n";
}

} // namespace AuthorScenes

#endif // MC_AUTHOR_SCENES_ONCE
