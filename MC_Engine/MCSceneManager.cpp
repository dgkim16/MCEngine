#include "MCSceneManager.h"
#include "MCEngine.h"
#include "MCScene.h"
#include "Common/d3dUtil.h"   // ThrowIfFailed
#include "RenderItem.h"        // RenderLayer count
#include <cassert>             // Save's path-mismatch guard
#include <filesystem>          // RecordAssetRedirect uses std::filesystem::rename
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>       // LoadAssetRedirects cycle detection

MCSceneManager::MCSceneManager(MCEngine& e) : mEngine(e) {}
MCSceneManager::~MCSceneManager() = default;

void MCSceneManager::LoadFromJson(const std::string& name, const std::string& path) {
	// throw std::runtime_error("MCSceneManager::LoadFromJson not implemented (D8 Step 6a)");
	nlohmann::json j = mEngine.Migrator().LoadAndMigrate(path);
	if (j.at("sceneName").get<std::string>() != name)
		throw std::runtime_error("LoadFromJson: file '" + path + "' has sceneName '" +
			j["sceneName"].get<std::string>() + "', expected '" + name + "'");

	auto scene = std::make_unique<MCScene>(name);

	// MCScene::LoadFromJson walks the seven keys (meshes / materialHandles /
	// textureHandles / specialPointers / renderItems / modules; version is
	// metadata). The modules walk dispatches by type via inline if/else
	// (D9 0.5c collapsed the prior registry + bootstrap stack — one user).
	scene->LoadFromJson(mEngine, j);
	scene->loaded = true;

	mScenes[name] = std::move(scene);
	mScenePaths[name] = path;
}

void MCSceneManager::LoadAll(const std::vector<std::pair<std::string, std::string>>& pairs) {
	for (const auto& [name, path] : pairs) {
		LoadFromJson(name, path);
	}
}

void MCSceneManager::LoadAssetRedirects(const std::string& path) {
	namespace fs = std::filesystem;
	if (!fs::exists(path)) {
		// First boot — no asset_redirects file yet. Initialize an empty redirects array
		// so RecordAssetRedirect's first append has a container to push into.
		mAssetRedirectsJson = nlohmann::json{ {"redirects", nlohmann::json::array()} };
		OutputDebugStringA("[redirects] no file; starting empty\n");
		return;
	}
	std::ifstream(path) >> mAssetRedirectsJson;
	if (!mAssetRedirectsJson.contains("redirects"))
		mAssetRedirectsJson["redirects"] = nlohmann::json::array();

	// D9 Step 2c — duplicate `from` is ambiguous, refuse loudly. Without this,
	// the second entry silently overwrote the first under operator[] semantics.
	for (auto& entry : mAssetRedirectsJson["redirects"]) {
		auto from = std::stoull(entry["from"].get<std::string>());
		auto to   = std::stoull(entry["to"].get<std::string>());
		auto [it, inserted] = mAssetRedirects.try_emplace(from, to);
		if (!inserted) {
			throw std::runtime_error(path + ": ambiguous redirect from " +
				std::to_string(from) + ": already maps to " +
				std::to_string(it->second) + ", new target " +
				std::to_string(to));
		}
	}

	// D9 Step 2a — eager cycle detection. Walk each chain; revisit = cycle.
	// Without this, ResolveAssetRedirect's while-loop infinite-loops on A->B->A.
	// O(N^2) worst case; N is small.
	for (const auto& [start, _] : mAssetRedirects) {
		std::unordered_set<std::uint64_t> seen;
		auto cur = start;
		while (true) {
			if (!seen.insert(cur).second) {
				throw std::runtime_error(path + ": redirect cycle detected at " +
					std::to_string(cur) + " (chain started at " +
					std::to_string(start) + ")");
			}
			auto it = mAssetRedirects.find(cur);
			if (it == mAssetRedirects.end()) break;
			cur = it->second;
		}
	}

	char buf[64];
	std::snprintf(buf, sizeof(buf), "[redirects] loaded %zu entries\n", mAssetRedirects.size());
	OutputDebugStringA(buf);
}

// D9 Step 2b — dangling-target detection. Each redirect's `to` must be a
// handle registered in the manager indicated by `kind`. Called from
// MCEngine::Initialize after LoadAll so procedural meshes declared in
// scene JSON are present.
void MCSceneManager::ValidateRedirects() const {
	for (const auto& entry : mAssetRedirectsJson["redirects"]) {
		const auto kind = entry.at("kind").get<std::string>();
		const auto to   = std::stoull(entry.at("to").get<std::string>());
		bool exists = false;
		if      (kind == "material") exists = mEngine.Materials().IsLoaded(to);
		else if (kind == "mesh")     exists = mEngine.Meshes().IsLoaded(to);
		else if (kind == "texture")  exists = mEngine.Textures().IsLoaded(to);
		else throw std::runtime_error("redirect: unknown kind '" + kind + "'");
		if (!exists) {
			throw std::runtime_error("dangling redirect target " +
				std::to_string(to) + " for kind '" + kind + "'");
		}
	}
}

void MCSceneManager::SetScenePath(const std::string& name, const std::string& path) {
	if (!mScenes.count(name))
		throw std::runtime_error("SetScenePath: unknown scene '" + name + "'");
	mScenePaths[name] = path;
}

std::string MCSceneManager::GetScenePath(const std::string& name) const {
	auto it = mScenePaths.find(name);
	return it == mScenePaths.end() ? std::string{} : it->second;
}

void MCSceneManager::EraseScene(const std::string& name) {
	if (mActive && mActive->name == name)
		throw std::runtime_error("EraseScene: '" + name + "' is active; ClearActive first");
	mScenes.erase(name);
	mScenePaths.erase(name);
}

void MCSceneManager::Save(const std::string& name, const std::string& path) const {
	auto* s = Get(name);
	if (!s) throw std::runtime_error("Save: scene '" + name + "' not registered");

	// Belt-and-suspenders: catch the class of bug where a caller passes a path
	// that doesn't match this scene's registered path (e.g. stale mCurrentScenePath
	// after a scene Switch). The cost of getting this wrong is overwriting
	// somebody else's scene JSON with this scene's content — silent corruption
	// of user data. Fires loudly the moment paths desync.
	auto it = mScenePaths.find(name);
	if (it != mScenePaths.end()) {
		std::filesystem::path want = it->second;
		std::filesystem::path got  = path;
		std::error_code ec;
		// weakly_canonical handles relative vs absolute + . / .. without requiring
		// the file to exist (path may be the target of the about-to-happen write).
		auto wantN = std::filesystem::weakly_canonical(want, ec); if (ec) wantN = want;
		auto gotN  = std::filesystem::weakly_canonical(got,  ec); if (ec) gotN  = got;
		assert(wantN == gotN
			&& "MCSceneManager::Save: path argument does not match the registered "
			   "path for this scene. Likely cause: stale path cached outside the "
			   "scene manager. Use SaveActiveToRegisteredPath() instead.");
	}

	std::ofstream(path) << s->ToJson().dump(2);
}

// D8 Step 2 — moved verbatim from MCEngine::SaveActiveSceneToJson (MCEngine.cpp:200-211 pre-D8).
// SAVE — read-only. Walks the active scene's render items and emits one JSON file.
// Does not modify engine state. D8 Step 6b extends this to walk all eight top-level keys
// (materialHandles / textureHandles / meshes / externalMeshes / specialPointers / modules).
void MCSceneManager::SaveActive(const std::string& path) const {
	if (!mActive) throw std::runtime_error("SaveActive: no active scene");
	Save(mActive->name, path);
}

// W4A D1 — same lookup Reload() does at line 213. Keeps mScenePaths private.
void MCSceneManager::SaveActiveToRegisteredPath() const {
	if (!mActive) throw std::runtime_error("SaveActiveToRegisteredPath: no active scene");
	auto it = mScenePaths.find(mActive->name);
	if (it == mScenePaths.end())
		throw std::runtime_error(
			"SaveActiveToRegisteredPath: no path registered for scene '" + mActive->name + "'");
	Save(mActive->name, it->second);
}

void MCSceneManager::SetActiveForBoot(const std::string& name) {
	auto it = mScenes.find(name);
	if (it == mScenes.end()) throw std::runtime_error("SetActiveForBoot: '" + name + "' not registered");
	mActive = it->second.get();
}

// Transitional shim. D8 Step 8c replaces this with LoadAll(...) reading scene_*.json.
void MCSceneManager::RegisterScene(std::unique_ptr<MCScene> scene) {
	if (!scene) throw std::runtime_error("RegisterScene: null scene");
	std::string key = scene->name;
	if (key.empty()) throw std::runtime_error("RegisterScene: scene has no name");
	if (mScenes.count(key)) throw std::runtime_error("RegisterScene: duplicate name '" + key + "'");
	mScenes[key] = std::move(scene);
}

// D8 Step 1c+1d — full Switch body, ported from MCEngine::SwitchScene
// (MCEngine.cpp:1288-1359 pre-D8) with the move-dance dropped.
//
// In v1, render items moved engine↔scene at every Switch. v2 keeps render items on the scene
// permanently; engine read sites query through mSceneManager.GetActive()->allRitems / ->layers
// directly (D8 Step 1d). The cost is one extra pointer hop per read; the win is no copy on
// every scene switch and no risk of the engine-side and scene-side stores drifting.
void MCSceneManager::Switch(const std::string& name) {
	if (mActive && mActive->name == name) return;

	mEngine.FlushCommandQueue();

	// --- Reset special render item pointers ---
	mEngine.SetModelRitem(nullptr);
	mEngine.SetReflectedModelRitem(nullptr);
	mEngine.SetShadowedModelRitem(nullptr);
	mEngine.SetTessellatedRitem(nullptr);

	if (mActive)
		mActive->Deactivate(mEngine);

	// --- Switch to new scene ---
	// D9 Step 6c — explicit find+throw so the error names the missing scene.
	// (Pre-D9 used mScenes.at(name) which throws std::out_of_range with a
	// non-descriptive "invalid map<K,T> key" message.)
	auto it = mScenes.find(name);
	if (it == mScenes.end())
		throw std::runtime_error("Switch: scene '" + name + "' not loaded");
	mActive = it->second.get();
	if (!mActive->loaded)
		throw std::runtime_error("Switch: scene '" + name + "' is not loaded — LoadAll must run before Switch");

	// --- Reassign ObjCBIndex on the scene's render items ---
	auto sa = mEngine.sceneAccess();
	for (int i = 0; i < (int)mActive->allRitems.size(); i++)
		mActive->allRitems[i]->ObjCBIndex = i;
	sa.TotalObjects = (int)mActive->allRitems.size();

	// --- Rebuild per-frame resources for new counts ---
	mEngine.RebuildFrameResources();

	// --- Mark everything dirty ---
	mEngine.DirtyAllRenderItems();
	for (auto& [h, v] : mEngine.Materials().All())
		v->NumFramesDirty = gNumFrameResources;

	// --- MCScene-specific camera / lighting / fog ---
	mActive->Activate(mEngine);
	mActive->RebindCachedPointers(mEngine);   // D-3: caller orders both, Activate does NOT auto-Rebind.
	mEngine.PreInitObjectCBs();


	// --- Reset selected ---
	mEngine.mSelectedItemIndex = -1;
	std::cout << "Switch -> '" << name << "': " << sa.TotalObjects << " render items\n";
	OutputDebugStringA(("Switch -> '" + name + "'\n").c_str());
}

void MCSceneManager::ConsumePendingSwitch() {
	if (mPendingSwitch.empty()) return;
	std::string n = std::move(mPendingSwitch);
	mPendingSwitch.clear();
	Switch(n);
}

// D9 Step 5a — full-scene reload from disk. Save first to flush in-memory ImGui
// edits to module params (e.g. MCGrassCullingModule::grassCountWidth) before
// drop-and-reload — without the Save, those edits are silently destroyed.
void MCSceneManager::Reload(bool save) {
	if (!mActive) throw std::runtime_error("Reload: no active scene");
	const std::string activeName = mActive->name;
	const std::string path = mScenePaths.at(activeName);
	// mEngine.mSelectedItemIndex = -1; // don't need this for now.
	if (save) {
		Save(activeName, path);
	}

	mActive->Deactivate(mEngine);
	mActive = nullptr;
	mScenes.erase(activeName);   // unique_ptr destructor → ~MCScene → Clear → UnregisterUsedBy + ObjCB release

	LoadFromJson(activeName, path);
	Switch(activeName);            // re-activate, RebindCachedPointers
}

MCScene* MCSceneManager::Get(const std::string& name) {
	auto it = mScenes.find(name);
	return it == mScenes.end() ? nullptr : it->second.get();
}

const MCScene* MCSceneManager::Get(const std::string& name) const {
	auto it = mScenes.find(name);
	return it == mScenes.end() ? nullptr : it->second.get();
}


// D8 Step 2 — moved from MCEngine::TestLoadActiveSceneFromJson (MCEngine.cpp:218-263 pre-D8).
// LOAD — destructive. Tears down every render item in the active scene, then reconstructs
// them from JSON. Materials and geometries are not touched (they live in mMaterialManager /
// mMeshSourceManager; this load resolves references by handle). Cached engine pointers are
// reset before wipe and re-bound after load.
void MCSceneManager::TestLoadActive(const std::string& path) {
	if (!mActive) throw std::runtime_error("TestLoadActive: no active scene");
	MCScene& scene = *mActive;
	auto j = mEngine.Migrator().LoadAndMigrate(path);
	if (j.at("sceneName").get<std::string>() != scene.name) {
		throw std::runtime_error(
			"TestLoadActive: file is for scene '" +
			j["sceneName"].get<std::string>() + "', active scene is '" + scene.name + "'");
	}

	// 1. Reset cached engine pointers BEFORE wipe — they're about to dangle.
	mEngine.SetModelRitem(nullptr);
	mEngine.SetReflectedModelRitem(nullptr);
	mEngine.SetShadowedModelRitem(nullptr);
	mEngine.SetTessellatedRitem(nullptr);

	// 2. Release every ObjCBIndex back to the free list. Walk before wipe.
	auto sa = mEngine.sceneAccess();
	for (auto& uptr : scene.allRitems) {
		sa.ObjCBFreeList.Release(uptr->ObjCBIndex);
	}

	// 3. Wipe all scene-side render-item containers.
	scene.allRitems.clear();
	for (auto& layer : scene.layers) layer.clear();
	scene.nameToRitem.clear();

	// 4. Reload every item from JSON via the engine-side insertion path.
	//    LoadRenderItemFromJson resolves Mat/Geo against mMaterialManager / mMeshSourceManager
	//    and inserts into scene.allRitems + scene.layers[i] + scene.nameToRitem.
	for (const auto& itemJson : j.at("renderItems")) {
		mEngine.LoadRenderItemsFromJson(itemJson, scene);
	}

	// 5. Reactivate. D-3: caller orders Activate + RebindCachedPointers explicitly.
	scene.Activate(mEngine);
	scene.RebindCachedPointers(mEngine);

	// D9 Step 5a — module OnLoad fires inside scene.LoadFromJson above for
	// modules attached during the load. Instanced GPU buffers are rebuilt
	// there. Earlier warning about "render empty until next scene switch" is
	// no longer accurate; deleted.
}

// TEST — wire to the ImGui button. Calls SaveActive, then TestLoadActive, then prints OK.
void MCSceneManager::TestRoundTripActiveScene() {
	SaveActive("scene_test_full.json");
	TestLoadActive("scene_test_full.json");
	OutputDebugStringA("[round-trip] save+load complete; visual check.\n");
}

#include <chrono>
#include <format>
std::string CurrentDateString() {
	auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
	auto ymd = std::chrono::year_month_day{ today };
	return std::format("{:%Y-%m-%d}", ymd);
}

void MCSceneManager::RecordAssetRedirect(const std::string& kind, std::uint64_t from,
                                         std::uint64_t to, const std::string& reason) {
	nlohmann::json entry = {
		{"kind", kind}, {"from", std::to_string(from)}, {"to", std::to_string(to)},
		{"reason", reason}, {"date", CurrentDateString()}
	};
	mAssetRedirectsJson["redirects"].push_back(entry);
	mAssetRedirects[from] = to;

	// Atomic write per the load-bearing comment above.
	auto tmp = std::filesystem::path("assets/asset_redirects.json.tmp");
	auto target = std::filesystem::path("assets/asset_redirects.json");
	std::ofstream(tmp) << mAssetRedirectsJson.dump(2);
	std::filesystem::rename(tmp, target);
}

std::uint64_t MCSceneManager::ResolveAssetRedirect(std::uint64_t handle) const {
	// D8 Step 6c chain-follow lands later. For now, identity — no redirects recorded.
	auto cur = handle;
	auto it = mAssetRedirects.find(cur);
	while (it != mAssetRedirects.end()) {
		cur = it->second;
		it = mAssetRedirects.find(cur);
	}
	return cur;
}
