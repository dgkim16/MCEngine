#pragma once
#include "Legacy/RenderItem.h"
#include "Renderer/D3D12/d3dUtil.h"
#include "Renderer/D3D12/MC_Types.h"
#include "Legacy/MCMaterial.h"
#include "Core/AssetIdentity.h"
#include "Legacy/MCMeshSource.h"        // MeshSourceDecl::source
#include "Legacy/MCSceneModule.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <memory>
#include <string>
#include <stdexcept>
#include <cassert>
#include <nlohmann/json.hpp>


class MCEngine;

// D8 Step 3 renamed this from the old `Scene` class. Until Step 8e the v1
// subclasses (Scene_Ch7, Scene_Empty, Scene_grass) inherit from MCScene and
// override the virtual lifecycle below. Step 5 replaces the virtual lifecycle
// with concrete bodies that walk `modules`; Step 8 adds the v2 data fields
// (handle lists, modules, externalMeshes, specialPointers, meshSourceDecls);
// Step 8e deletes the subclasses and MCScene becomes the only scene class.
class MCScene
{
public:
	MCScene() = default;
	explicit MCScene(std::string name) : name(std::move(name)) {}
	virtual ~MCScene();   // out-of-line; releases ObjCBs via Clear if mEngine still valid.

	// JSON-loaded scene path. Not virtual; subclasses don't override this.
	void LoadFromJson(MCEngine& engine, const nlohmann::json& j);

	// v1 subclass extension points; default bodies walk `modules`. v1 subclasses
	// (Scene_Ch7/Scene_Empty/Scene_grass) override these. After Step 8e (subclass
	// deletion + virtual Load removal), these can become non-virtual concrete.
	// virtual void Load(MCEngine& engine) = 0;            // pure virtual; subclass must implement
	virtual void Unload(MCEngine& engine);              // walks modules.OnUnload; call GPU-idle, right before destroy
	virtual void OnImGui(MCEngine& engine);             // walks modules.OnImGui
	void ResetSceneResources();                          // releases module GPU resources before device dies (engine-dtor path)

	nlohmann::json ToJson() const;

	/*
	 * Clear(MCEngine& engine) explanation :
	 * 1. Releases ObjCBIndex slots back to mObjCBFreeList.
	 * 2. Destroys render items + clears nameToRitem + layers.
	 *    LOAD-BEARING ORDER: render items destroyed BEFORE handle
	 *    unregistration so MCMeshSourceManager's drop-on-empty
	 *    (D9 9B) cannot dangle r->Geo pointers. See MCScene.cpp.
	 * 3. Calls UnregisterUsedBy on each material/mesh/texture handle.
	 *    Mesh source may be dropped here if its mUsedBy becomes empty.
	 * 4. Does NOT call manager Clear() — that's engine-shutdown
	 *    teardown, distinct from scene-clear.
	*/
	void Clear(MCEngine& engine);

	// if a single module type is attached to a scene multiple times 
	// (e.g., two MCAudioReactiveModule instances with different params), 
	// GetModule<T>() returns the first match.
	// So, If you need multi-of-same-type later, add GetModules<T>() -> std::vector<T*> later
	template <typename T>
	T* GetModule() {
		for (auto& m : modules)
			if (auto* p = dynamic_cast<T*>(m.get())) return p;
		return nullptr;
	}

	void AddModule(std::unique_ptr<MCSceneModule> m) { modules.push_back(std::move(m)); }

	std::string name;
	bool loaded = false;

	// Non-owning back-pointer to the engine that loaded this scene. Set by
	// LoadFromJson; nulled by MCEngine's destructor before member teardown so
	// ~MCScene does not touch a dying engine. Used by ~MCScene + Clear() to
	// release each render item's ObjCBIndex back to mObjCBFreeList.
	MCEngine* mEngine = nullptr;

	// data
	std::vector<std::unique_ptr<RenderItem>>  allRitems;
	std::set<RenderItem*>                     layers[(int)RenderLayer::Count];
	std::unordered_map<std::string, RenderItem*> nameToRitem;

	// handle lists
	std::vector<MaterialHandle> materialHandles;
	std::vector<MeshHandle>     meshHandles;
	std::vector<TextureHandle>  textureHandles;

	// Inline procedural mesh declarations
	/*
	struct MeshSourceDecl { std::string displayName; MCMeshSource source; };
	std::vector<MeshSourceDecl> meshSourceDecls;
	*/

	// other
	std::unordered_map<std::string, std::string> specialPointers;
	std::vector<std::unique_ptr<MCSceneModule>>  modules;

	// Deprectaed, must be removed
	std::unordered_map<std::string, std::unique_ptr<MCMaterial>>   materials;
	std::unordered_map<int, std::string>                           materialIndexTracker;
};
