#include "MCEngine.h"
#include "MCScene.h"
#include "MCGrassCullingModule.h"   // inline module factory in modules walk (D9 0.5c)

// Called from MCEngine's destructor (MCEngine.cpp:91-93) before md3dDevice.Reset.
// Modules own MCBufferResource members + std::array<UploadBuffer<...>> arrays
// holding ID3D12Resource ComPtrs; if those don't release before the device,
// ReportLiveObjects flags them as leaks (D3D12 STATE_CREATION #274 LIVE_DEVICE).
// Clearing modules here invokes their destructors → MCBufferResource ComPtrs
// release → device refcount drops to its expected level. Render items don't
// own GPU resources directly (they reference manager-owned MCMeshGeometry +
// MCMaterial), so allRitems can stay until the scene itself is destroyed.
void MCScene::ResetSceneResources() {
	modules.clear();
}

// Called from ~MCScene and (eventually) from MCSceneManager::Reload. Idempotent:
// after Clear, allRitems is empty so a re-call is a no-op.
void MCScene::Clear(MCEngine& engine) {
	auto sa = engine.sceneAccess();
	for (auto& ri : allRitems) {
		if (ri && ri->ObjCBIndex >= 0) {
			sa.ObjCBFreeList.Release(ri->ObjCBIndex);
			ri->ObjCBIndex = -1;
		}
	}

	// Renderitems destroyed FIRST. otherwise,
	// drop-on-empty inside MCMeshSourceManager::UnregisterUsedBy 
	// would dangle r->Geo pointers
	allRitems.clear();
	for (auto& l : layers) l.clear();
	nameToRitem.clear();

	// Unregister handles. 
	// MeshSourceManager may drop the source when `mUsedBy[h]` becomes empty
	// Renderitem's `r->Geo` pointers don't survive renderitme destroy above, so the drop is safe.
	for (auto h : materialHandles) engine.Materials().UnregisterUsedBy(h, name);
	for (auto h : meshHandles)     engine.Meshes().UnregisterUsedBy(h, name);
	for (auto h : textureHandles)  engine.Textures().UnregisterUsedBy(h, name);

	
	materialHandles.clear();
	meshHandles.clear();
	textureHandles.clear();
	specialPointers.clear();
	modules.clear();
}

// RAII destructor. mEngine is non-null while the engine is alive; nulled by
// MCEngine::~MCEngine before member teardown to keep this dtor from touching
// a dying engine during the unique_ptr<MCScene> destruction inside
// mSceneManager.~MCSceneManager().
MCScene::~MCScene() {
	if (mEngine) Clear(*mEngine);
}

void MCScene::Activate(MCEngine& e) { for (auto& m : modules) m->OnActivate(e, *this); }
void MCScene::Deactivate(MCEngine& e) { for (auto& m : modules) m->OnDeactivate(e, *this); }
void MCScene::Update(MCEngine& e, float dt) { for (auto& m : modules) m->OnUpdate(e, *this, dt); }
void MCScene::Draw(MCEngine& e) { for (auto& m : modules) m->OnDraw(e, *this); }
void MCScene::OnImGui(MCEngine& e) { for (auto& m : modules) m->OnImGui(e, *this); }

// D8 Step 8A3 — full body, eight top-level keys. The drop-merging design
// removes the "externalMeshes" key that earlier drafts of the plan included;
// also note that `void MCScene::LoadFromJson` sets `mEngine = &engine` so the
// scene's destructor can release ObjCBIndex slots without an explicit Clear call.
// every mesh under v2 lives in the "meshes" array as {name, kind, params}.
nlohmann::json MCScene::ToJson() const {
	nlohmann::json j;
	j["version"]   = 1;
	j["sceneName"] = name;

	// materialHandles / textureHandles: stringified uint64. Round-trips via
	// std::stoull on the load side (LoadFromJson at MCScene.cpp's eventual
	// 8A5 walker; OLD body at MCScene.cpp:75-79 for the existing pattern).
	j["materialHandles"] = nlohmann::json::array();
	for (auto h : materialHandles) j["materialHandles"].push_back(std::to_string(h));
	j["textureHandles"] = nlohmann::json::array();
	for (auto h : textureHandles) j["textureHandles"].push_back(std::to_string(h));

	// meshes: each entry is {name, kind, params:{...kind-specific named fields...}}.
	// MCMeshSource::KindToString and ::ParamsToJson are the inverses of
	// ParseKind / ParseParams, defined inline alongside them.
	j["meshes"] = nlohmann::json::array();
	if (mEngine) {
		for (auto h : meshHandles) {
			auto* src = mEngine->Meshes().GetSource(h);
			if (!src) continue;   // dropped/missing — shouldn't happen post-load, log if it does.
			nlohmann::json m;
			m["name"] = src->geometry ? src->geometry->Name : std::string{};
			m["kind"] = MCMeshSource::KindToString(src->kind);
			m["params"] = MCMeshSource::ParamsToJson(src->params);
			j["meshes"].push_back(std::move(m));
		}
	}

	// renderItems: nlohmann ADL serialization. SubmeshName field dropped
	// in 8A3 — single-level mesh reference under drop-merging.
	j["renderItems"] = nlohmann::json::array();
	for (const auto& uptr : allRitems) j["renderItems"].push_back(nlohmann::json(*uptr));

	// specialPointers: string→string map; nlohmann handles unordered_map directly.
	j["specialPointers"] = specialPointers;

	// modules: each writes {type, params}. Module FromJson reads the params object.
	j["modules"] = nlohmann::json::array();
	for (const auto& m : modules) {
		nlohmann::json mod;
		mod["type"] = m->TypeName();
		nlohmann::json params;
		m->ToJson(params);
		mod["params"] = std::move(params);
		j["modules"].push_back(std::move(mod));
	}

	return j;
}

// D8 Step 8A5 — full seven-key walker. Replaces the 8A3 modules-only stub.
// Order matters: render items resolve their (mesh, material, texture) handles
// against the engine's managers, so meshes/materials/textures must land first.
// Every key is optional via j.find — a scene that declares no modules / no
// meshes / etc. simply skips the corresponding walk.
void MCScene::LoadFromJson(MCEngine& engine, const nlohmann::json& j) {
	// Capture engine pointer so ~MCScene can release ObjCBIndex slots back to
	// mObjCBFreeList. MCEngine::~MCEngine nulls this before member teardown.
	mEngine = &engine;

	// 1. meshes — register every kind via MCMeshSourceManager. Idempotent on
	//    duplicate displayName + matching kind, so reload paths and cross-scene
	//    name collisions (which shouldn't happen given per-scene prefixing)
	//    fall through cleanly.
	if (auto it = j.find("meshes"); it != j.end()) {
		for (const auto& meshJson : *it) {
			MCMeshSource src;
			src.kind = MCMeshSource::ParseKind(meshJson.at("kind").get<std::string>());
			MCMeshSource::ParseParams(src, meshJson.at("params"));
			const auto displayName = meshJson.at("name").get<std::string>();

			auto h = engine.Meshes().Register(displayName, std::move(src));
			meshHandles.push_back(h);
			engine.Meshes().RegisterUsedBy(h, name);
		}
	}

	// 2. materialHandles — stringified uint64; track usage so cleanup knows
	//    which scenes hold this material. ResolveAssetRedirect collapses any
	//    rename chain so RegisterUsedBy attributes usage to the canonical handle.
	if (auto it = j.find("materialHandles"); it != j.end()) {
		for (const auto& s : *it) {
			auto h = engine.Scenes().ResolveAssetRedirect(std::stoull(s.get<std::string>()));
			materialHandles.push_back(h);
			engine.Materials().RegisterUsedBy(h, name);
		}
	}

	// 3. textureHandles — same shape as materials.
	if (auto it = j.find("textureHandles"); it != j.end()) {
		for (const auto& s : *it) {
			auto h = engine.Scenes().ResolveAssetRedirect(std::stoull(s.get<std::string>()));
			textureHandles.push_back(h);
			engine.Textures().RegisterUsedBy(h, name);
		}
	}

	// 4. specialPointers — string→string map; nlohmann handles unordered_map directly.
	if (auto it = j.find("specialPointers"); it != j.end()) {
		for (auto& [k, v] : it->items()) {
			specialPointers[k] = v.get<std::string>();
		}
	}

	// 5. renderItems — engine-side per-item resolution. Each call resolves
	//    materialHandle / meshHandle against the managers populated above and
	//    pushes into allRitems / layers / nameToRitem.
	if (auto it = j.find("renderItems"); it != j.end()) {
		for (const auto& itemJson : *it) {
			engine.LoadRenderItemsFromJson(itemJson, *this);
		}
	}

	// 6. modules — inline factory dispatch (D9 0.5c collapsed the registry +
	//    bootstrap stack: one module type, no plugin DLLs, no self-registration,
	//    no dependency ordering). Re-extract a registry the day a second type
	//    lands and provides enough information to design the abstraction.
	if (auto it = j.find("modules"); it != j.end()) {
		for (const auto& entry : *it) {
			const auto type = entry.at("type").get<std::string>();
			std::unique_ptr<MCSceneModule> m;
			if (type == "MCGrassCulling") m = std::make_unique<MCGrassCullingModule>();
			else throw std::runtime_error("unknown module type: " + type);
			m->FromJson(entry.at("params"));
			modules.push_back(std::move(m));
		}
	}

	// 7. OnLoad — fire after all data is in place. Modules can read scene state
	//    (meshes, materials, render items, other modules) during OnLoad; this is
	//    the canonical hook for cross-data setup that depends on the full scene
	//    being assembled.
	for (auto& m : modules) m->OnLoad(engine, *this);
}

void MCScene::RebindCachedPointers(MCEngine& engine) {
	auto resolve = [&](const std::string& key) -> RenderItem* {
		auto it = specialPointers.find(key);
		if (it == specialPointers.end()) return nullptr;
		auto rit = nameToRitem.find(it->second);
		return rit == nameToRitem.end() ? nullptr : rit->second;
		};
	engine.SetModelRitem(resolve("modelRitem")); // these should eventually be removed
	engine.SetReflectedModelRitem(resolve("reflectedModelRitem"));
	engine.SetShadowedModelRitem(resolve("shadowedModelRitem"));
	engine.SetTessellatedRitem(resolve("tessellatedRitem"));
}

RenderItem* MCScene::AddRenderItem(std::unique_ptr<RenderItem> r, RenderLayer layer) {
	assert(!r->Name.empty() && "RenderItem must have a name before AddRenderItem");
	auto [it, inserted] = nameToRitem.try_emplace(r->Name, r.get());
	if (!inserted) {
		throw std::runtime_error("Duplicate RenderItem name in scene '" + name + "': " + r->Name);
	}
	r->Layers |= LayerBit(layer);
	layers[(int)layer].insert(r.get());
	RenderItem* raw = r.get();
	allRitems.push_back(std::move(r));
	return raw;
}

RenderItem* MCScene::AddRenderItem(std::unique_ptr<RenderItem> r,
	std::initializer_list<RenderLayer> layerList) {
	assert(!r->Name.empty() && "RenderItem must have a name before AddRenderItem");
	assert(layerList.size() > 0 && "RenderItem must be assigned to at least one layer");
	auto [it, inserted] = nameToRitem.try_emplace(r->Name, r.get());
	if (!inserted) {
		throw std::runtime_error("Duplicate RenderItem name in scene '" + name + "': " + r->Name);
	}
	for (RenderLayer layer : layerList) {
		r->Layers |= LayerBit(layer);
		layers[(int)layer].insert(r.get());
	}
	RenderItem* raw = r.get();
	allRitems.push_back(std::move(r));
	return raw;
}

RenderItem* MCScene::AddRenderItem(std::unique_ptr<RenderItem> r,
	const std::vector<RenderLayer>& layerList) {
	assert(!r->Name.empty() && "RenderItem must have a name before AddRenderItem");
	assert(!layerList.empty() && "RenderItem must be assigned to at least one layer");
	auto [it, inserted] = nameToRitem.try_emplace(r->Name, r.get());
	if (!inserted) {
		throw std::runtime_error("Duplicate RenderItem name in scene '" + name + "': " + r->Name);
	}
	for (RenderLayer layer : layerList) {
		r->Layers |= LayerBit(layer);
		layers[(int)layer].insert(r.get());
	}
	RenderItem* raw = r.get();
	allRitems.push_back(std::move(r));
	return raw;
}