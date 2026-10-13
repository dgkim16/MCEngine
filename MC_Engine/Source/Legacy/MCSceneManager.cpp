#include "Legacy/MCSceneManager.h"
#include "Core/LoggingSystem.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"
#include "Renderer/D3D12/d3dUtil.h"   // ThrowIfFailed
#include "Legacy/RenderItem.h"        // RenderLayer count
#include <filesystem>          // RecordAssetRedirect uses std::filesystem::rename
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>       // LoadAssetRedirects cycle detection

MCSceneManager::MCSceneManager(MCEngine& e) : mEngine(e) {}
MCSceneManager::~MCSceneManager() = default;

void MCSceneManager::ClearActive() { mActive.reset(); }

void MCSceneManager::SetScenePath(const std::string& name, const std::string& path) {
	mScenePaths[name] = path;
}

std::string MCSceneManager::GetScenePath(const std::string& name) const {
	auto it = mScenePaths.find(name);
	return it == mScenePaths.end() ? std::string{} : it->second;
}

void MCSceneManager::LoadAssetRedirects(const std::string& path) {
	namespace fs = std::filesystem;
	mAssetRedirectsPath = path;   // RecordAssetRedirect writes back here, even when the file is missing
	if (!fs::exists(path)) {
		mAssetRedirectsJson = nlohmann::json{ {"redirects", nlohmann::json::array()} };
		MC::LoggingSystem::Info("[redirects] no file; starting empty");
		return;
	}
	std::ifstream(path) >> mAssetRedirectsJson;
	if (!mAssetRedirectsJson.contains("redirects"))
		mAssetRedirectsJson["redirects"] = nlohmann::json::array();

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

	MC::LoggingSystem::Info("[redirects] loaded {} entries", mAssetRedirects.size());
}

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
			if (kind == "mesh") {
				MC::LoggingSystem::Warning("[redirects] WARN: mesh target {} not loaded (lives in an unloaded scene?)", to);
				continue;
			}
			throw std::runtime_error("dangling redirect target " +
				std::to_string(to) + " for kind '" + kind + "'");
		}
	}
}

void MCSceneManager::Save(const std::string& name, const std::string& path) const {
	// Only the active scene exists in memory — saving anything else is a caller bug.
	if (!mActive || mActive->name != name)
		throw std::runtime_error("Save: scene '" + name + "' is not the active scene");
	std::ofstream(path) << mActive->ToJson().dump(2);
}

void MCSceneManager::SaveActive(const std::string& path) const {
	if (!mActive) throw std::runtime_error("SaveActive: no active scene");
	Save(mActive->name, path);
}

void MCSceneManager::SaveActiveToRegisteredPath() const {
	if (!mActive) throw std::runtime_error("SaveActiveToRegisteredPath: no active scene");
	auto it = mScenePaths.find(mActive->name);
	if (it == mScenePaths.end())
		throw std::runtime_error(
			"SaveActiveToRegisteredPath: no path registered for scene '" + mActive->name + "'");
	Save(mActive->name, it->second);
}

// The single construction/destruction point for scenes at runtime.
// PRECONDITION: caller holds a scene-transition bracket (BeginSceneTransition —
// cmdlist open, GPU idle). Mesh geometry upload and module OnLoad record on the
// open cmdlist; the old scene's GPU resources release while the GPU is idle.
//
// Order is load-bearing:
//  1. Parse + migrate JSON FIRST — a missing/corrupt file throws before the
//     current scene is touched.
//  2. Destroy the old scene BEFORE constructing the new one. Same-name loads
//     (Reload, re-Open) share the UsedBy key: if both were alive, the old
//     scene's UnregisterUsedBy would cancel the new scene's registration and
//     drop-on-empty would free meshes the new scene references (AV at
//     MCMeshGeometry::VertexBufferView on the first frame).
//  3. A LoadFromJson throw after step 2 (e.g. unknown module type) resets the
//     partial scene and leaves no active scene; the caller reports and the user
//     picks another scene. Accepted tradeoff.
void MCSceneManager::LoadAndActivate(const std::string& name) {
	auto pit = mScenePaths.find(name);
	if (pit == mScenePaths.end())
		throw std::runtime_error("Switch: scene '" + name + "' is not in the catalog");

	nlohmann::json j = mEngine.Migrator().LoadAndMigrate(pit->second);
	if (j.at("sceneName").get<std::string>() != name)
		throw std::runtime_error("Switch: file '" + pit->second + "' has sceneName '" +
			j["sceneName"].get<std::string>() + "', expected '" + name + "'");

	// --- Destroy the outgoing scene ---
	// ~MCScene → Clear: releases ObjCBIndex slots, destroys render items, then
	// UnregisterUsedBy on every handle (managers drop-on-empty free unused GPU
	// assets — safe, GPU is idle inside the bracket).
	if (mActive) {
		mActive->Unload(mEngine);
		mActive.reset();
	}

	// --- Load the new scene from disk ---
	mActive = std::make_unique<MCScene>(name);
	try { mActive->LoadFromJson(mEngine, j); }   // seven-key walker; fires module OnLoad
	catch (...) {
		// No half-built scene. Its mesh and module uploads are already recorded on
		// the open cmdlist: run them first, so the reset below never frees a buffer
		// a pending copy still references, then reopen the caller's bracket.
		mEngine.EndSceneTransition();
		mEngine.BeginSceneTransition();
		mActive.reset();
		throw;
	}
	mActive->loaded = true;

	// --- Item count. Each item keeps the ObjCBIndex it was allocated at load ---
	auto sa = mEngine.sceneAccess();
	sa.TotalObjects = (int)mActive->allRitems.size();

	// --- Mark everything dirty ---
	mEngine.DirtyAllRenderItems();
	for (auto& [h, v] : mEngine.Materials().All())
		v->NumFramesDirty = gNumFrameResources;

	// --- Rebuild per-frame resources for new counts ---
	// Must run after LoadFromJson (module OnLoad populates per-instance data,
	// e.g. GrassInstances, that BuildFrameResources sizes GrassInstanceCB from)
	// and before PreInitObjectCBs (which writes into the new frame resources).
	mEngine.RebuildFrameResources();
	mEngine.PreInitObjectCBs();

	// --- Reset selected ---
	mEngine.mSelectedItemIndex = -1;
	std::cout << "Switch -> '" << name << "': " << sa.TotalObjects << " render items\n";
	MC::LoggingSystem::Info("Switch -> '{}'", name);
}

void MCSceneManager::Switch(const std::string& name) {
	if (mActive && mActive->name == name) return;
	LoadAndActivate(name);
}

void MCSceneManager::ConsumePendingSwitch() {
	if (mPendingSwitch.empty()) return;
	std::string n = std::move(mPendingSwitch);
	mPendingSwitch.clear();
	Switch(n);
}

// Destroy + re-read the active scene from disk. Save first (when asked) to
// flush in-memory ImGui edits to module params — without it, those edits are
// silently destroyed.
void MCSceneManager::Reload(bool save) {
	if (!mActive) throw std::runtime_error("Reload: no active scene");
	const std::string name = mActive->name;   // copy — mActive dies inside LoadAndActivate
	auto it = mScenePaths.find(name);
	if (it == mScenePaths.end())
		throw std::runtime_error("Reload: no path registered for scene '" + name + "'");
	if (save) Save(name, it->second);
	LoadAndActivate(name);
}

#include <chrono>
#include <format>
std::string CurrentDateString() {
	auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
	auto ymd = std::chrono::year_month_day{ today };
	return std::format("{:%Y-%m-%d}", ymd);
}

// Both checks run before anything changes, so a rejected redirect leaves the
// table and the file as they were.
void MCSceneManager::RecordAssetRedirect(const std::string& kind, std::uint64_t from,
                                         std::uint64_t to, const std::string& reason) {
	if (mAssetRedirectsPath.empty())
		throw std::runtime_error("RecordAssetRedirect: no redirect file was loaded");
	if (ResolveAssetRedirect(to) == from)
		throw std::runtime_error("RecordAssetRedirect: " + kind + " redirect " + std::to_string(from) +
			" -> " + std::to_string(to) + " would close a redirect cycle");

	nlohmann::json entry = {
		{"kind", kind}, {"from", std::to_string(from)}, {"to", std::to_string(to)},
		{"reason", reason}, {"date", CurrentDateString()}
	};
	mAssetRedirectsJson["redirects"].push_back(entry);
	mAssetRedirects[from] = to;

	// Atomic write to the file LoadAssetRedirects read.
	auto target = std::filesystem::path(mAssetRedirectsPath);
	auto tmp = target;
	tmp += ".tmp";
	std::ofstream(tmp) << mAssetRedirectsJson.dump(2);
	std::filesystem::rename(tmp, target);
}

// An acyclic table of n entries resolves in at most n hops, so needing one
// more means the chain loops.
std::uint64_t MCSceneManager::ResolveAssetRedirect(std::uint64_t handle) const {
	auto cur = handle;
	auto it = mAssetRedirects.find(cur);
	for (std::size_t hops = 0; it != mAssetRedirects.end(); ++hops) {
		if (hops == mAssetRedirects.size())
			throw std::runtime_error("ResolveAssetRedirect: redirect cycle reached from " +
				std::to_string(handle));
		cur = it->second;
		it = mAssetRedirects.find(cur);
	}
	return cur;
}
