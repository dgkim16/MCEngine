#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

class MCEngine;
class MCScene;

class MCSceneManager {
public:
	explicit MCSceneManager(MCEngine& engine);
	~MCSceneManager();   // defined in .cpp where MCScene is complete (unique_ptr<MCScene> dtor instantiation).
	void LoadFromJson(const std::string& name, const std::string& path);
	void LoadAll(const std::vector<std::pair<std::string, std::string>>& namePathPairs);

	void Save(const std::string& name, const std::string& path) const;
	void SaveActive(const std::string& path) const;
	// W4A D1 — convenience: save active scene to its registered path (mScenePaths
	// lookup happens internally; engine doesn't need to reach into private state).
	void SaveActiveToRegisteredPath() const;

	// Transitional shim during D8: scene init still calls RegisterScene(make_unique<Scene_X>())
	// before D8 Step 8c replaces it with LoadAll(...) reading scene_*.json.
	void RegisterScene(std::unique_ptr<MCScene> scene);

	void Switch(const std::string& name);
	void RequestSwitch(const std::string& name) { mPendingSwitch = name; }
	void Reload(bool save = false);

	// Boot-time path: MCEngine::Initialize loads the first scene with the cmd list still
	// open (subsequent Build* calls share the list). That path can't go through Switch
	// because Switch flushes. Initialize sets mActive directly via this setter, then runs
	// the load + move-into-engine inline, then calls Activate after final flush.
	void SetActiveForBoot(const std::string& name);
	void ClearActive() { mActive = nullptr; }

	MCScene* Get(const std::string& name);
	const MCScene* Get(const std::string& name) const;
	MCScene* GetActive() { return mActive; }
	const MCScene* GetActive() const { return mActive; }
	const std::unordered_map<std::string, std::unique_ptr<MCScene>>& All() const { return mScenes; }

	// Pending-switch consumed once per Update tick — mirrors v1's mPendingScene/SwitchScene flow.
	bool HasPendingSwitch() const { return !mPendingSwitch.empty(); }
	void ConsumePendingSwitch();

	// JSON I/O for the active scene (moved from MCEngine in D8 Step 2).
	// SaveActive is read-only on engine state. TestLoadActive is destructive: tears down
	// the active scene's render items, reloads them from JSON via engine.LoadRenderItemFromJson,
	// then re-runs scene.Activate. TestRoundTripActiveScene chains both for an ImGui smoke test.
	void TestLoadActive(const std::string& path);
	void TestRoundTripActiveScene();

	void LoadAssetRedirects(const std::string& path);
	void RecordAssetRedirect(const std::string& kind, std::uint64_t from, std::uint64_t to, const std::string& reason);
	std::uint64_t ResolveAssetRedirect(std::uint64_t handle) const;

	// Validate every redirect's `to` handle is registered in the manager indicated
	// by `kind`. Call after LoadAll so all assets (including procedural meshes
	// declared in scene JSON) are present. Throws on dangling target.
	void ValidateRedirects() const;

	void SetScenePath(const std::string& name, const std::string& path);
	std::string GetScenePath(const std::string& name) const;   // empty if scene not registered
	void EraseScene(const std::string& name);
private:
	MCEngine& mEngine;
	std::unordered_map<std::string, std::unique_ptr<MCScene>> mScenes;
	std::unordered_map<std::string, std::string> mScenePaths;
	MCScene* mActive = nullptr;
	std::string mPendingSwitch;

	// asset_redirects.json state
	nlohmann::json mAssetRedirectsJson;
	std::unordered_map<std::uint64_t, std::uint64_t> mAssetRedirects;  // from-handle → to-handle
};