#pragma once
#include "RenderItem.h"
#include "d3dUtil.h"
#include "MC_Types.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <memory>
#include <string>

class MCEngine;

struct MatProps { std::string matName, textureName; int renderLevel; };
static const std::vector<MatProps> s_matProps = {
	{"woodCrate",  "woodCrateTex",	(int)RenderLayer::Opaque},
	{"model",      "modelTex",		(int)RenderLayer::Opaque},
	{"model_ref",  "modelTex",		(int)RenderLayer::Reflected},
	{"gridFloor",  "gridTex",		(int)RenderLayer::Opaque},
	{"water",      "waterTex",		(int)RenderLayer::Transparent},
	{"fence",      "wirefenceTex",	(int)RenderLayer::AlphaTested},
	{"mirror",     "iceTex",		(int)RenderLayer::Transparent},
	{"brick",      "bricksTex",		(int)RenderLayer::Opaque},
	{"treeSprites","treeArrTex",	(int)RenderLayer::AlphaTested},
	{"tessellation","teapot_normal",(int)RenderLayer::OpaqueTessellated},
	{"default",	   "defaultTex",		(int)RenderLayer::Opaque},
	{"defaultOpaqueInst",	   "defaultTex",		(int)RenderLayer::OpaqueInstanced},
	{"grass",                  "defaultTex",		(int)RenderLayer::GrassInstanced},
	{"grassPlane",             "defaultTex",		(int)RenderLayer::Opaque},
};


class Scene
{
public:
	std::string name;
	bool loaded = false;

	virtual ~Scene() = default;

	// Called once on first activation to build GPU resources, materials, and render items.
	// The engine's command list is open when this is called.
	virtual void Load(MCEngine& engine) = 0;

	// Called each time this scene becomes the active scene (camera, lighting, fog setup).
	virtual void Activate(MCEngine& engine) {}

	// Called when this scene is deactivated (optional teardown hooks).
	virtual void Deactivate(MCEngine& engine) {}

	// Per-frame update hook (optional scene-specific logic).
	virtual void Update(MCEngine& engine, float dt) {}

	virtual void Scene_IMGUI(MCEngine& engine) {}

	virtual void ResetSceneResources() {}


	// --- Scene-owned data (moved into/out of the engine on switch) ---
	std::vector<std::unique_ptr<RenderItem>>  allRitems;
	std::set<RenderItem*>                     layers[(int)RenderLayer::Count];
	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> geometries;
	std::unordered_map<std::string, std::unique_ptr<Material>>     materials;
	std::unordered_map<int, std::string>                           materialIndexTracker;
};