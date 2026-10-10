#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

class MCEngine;
class MCScene;

// Load-on-switch scene manager. Exactly ONE scene is resident: mActive (owning).
// mScenePaths is the catalog (name → JSON path, strings only). Switch/Reload
// destroy the current scene and load the requested one from disk. Both record
// GPU uploads (mesh geometry, module OnLoad buffers), so every call MUST run
// inside MCEngine::Begin/EndSceneTransition — use RequestSwitch (consumed each
// Update inside a bracket), MCEngine::SwitchSceneNow, or ReloadActiveSceneNow.
class MCSceneManager {
public:
	explicit MCSceneManager(MCEngine& engine);
	~MCSceneManager();   // defined in .cpp where MCScene is complete (unique_ptr<MCScene> dtor instantiation).

	// Catalog only — no scene is loaded. Overwrites an existing entry.
	void SetScenePath(const std::string& name, const std::string& path);
	std::string GetScenePath(const std::string& name) const;   // empty if not cataloged
	const std::unordered_map<std::string, std::string>& Catalog() const { return mScenePaths; }

	// Destroys the active scene, loads `name` from its cataloged path, activates.
	// No-op if `name` is already active (use Reload to force re-read from disk).
	// Caller must hold a scene-transition bracket (cmdlist open, GPU idle).
	void Switch(const std::string& name);
	void RequestSwitch(const std::string& name) { mPendingSwitch = name; }
	// Destroy + re-read the active scene from its registered path (discards
	// in-memory edits unless save). Same bracket requirement as Switch.
	void Reload(bool save = false);

	void Save(const std::string& name, const std::string& path) const;  // name must be the active scene
	void SaveActive(const std::string& path) const;
	void SaveActiveToRegisteredPath() const;

	MCScene* GetActive() { return mActive.get(); }
	const MCScene* GetActive() const { return mActive.get(); }
	// Engine-shutdown teardown: destroys the owned active scene (caller nulls
	// scene->mEngine first so ~MCScene skips Clear on a dying engine).
	// Defined in the .cpp — unique_ptr reset needs the complete MCScene type.
	void ClearActive();

	// Pending-switch consumed once per Update tick, inside a transition bracket.
	bool HasPendingSwitch() const { return !mPendingSwitch.empty(); }
	void ConsumePendingSwitch();

	void LoadAssetRedirects(const std::string& path);
	void RecordAssetRedirect(const std::string& kind, std::uint64_t from, std::uint64_t to, const std::string& reason);
	std::uint64_t ResolveAssetRedirect(std::uint64_t handle) const;

	// Validate redirect targets. Materials/textures load globally at boot and are
	// checked strictly (throw). Mesh targets live in scene JSON and only exist
	// while their scene is loaded (load-on-switch) — dangling mesh targets warn
	// instead of throwing; the miss surfaces at scene load via handle resolution.
	void ValidateRedirects() const;

private:
	// The single construction point: destroys the current scene (Unload → ~MCScene
	// → Clear), loads `name` from mScenePaths, runs the activation tail (item
	// count, dirty-all, rebind, RebuildFrameResources, PreInitObjectCBs).
	void LoadAndActivate(const std::string& name);

	MCEngine& mEngine;
	std::unordered_map<std::string, std::string> mScenePaths;
	std::unique_ptr<MCScene> mActive;
	std::string mPendingSwitch;

	// asset_redirects.json state
	std::string mAssetRedirectsPath;   // the path LoadAssetRedirects was given; RecordAssetRedirect writes it
	nlohmann::json mAssetRedirectsJson;
	std::unordered_map<std::uint64_t, std::uint64_t> mAssetRedirects;  // from-handle → to-handle
};
