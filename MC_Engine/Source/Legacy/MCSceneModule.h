#pragma once
// Forward-decl only. Including MCScene.h here creates a circular include —
// MCScene.h includes MCSceneModule.h for unique_ptr<MCSceneModule>, and any
// TU that pulls MCSceneModule.h first would see an incomplete MCSceneModule
// when MCScene's implicit destructor instantiates. References to MCEngine /
// MCScene below are by reference — they only need forward declarations here.
class MCEngine;
class MCScene;
#include <nlohmann/json.hpp>

class MCSceneModule {
public:
	virtual ~MCSceneModule() = default;
	virtual const char* TypeName() const = 0;

	// Load-on-switch lifecycle: OnLoad fires at the end of MCScene::LoadFromJson,
	// inside a scene-transition bracket (cmdlist open, GPU idle) — build ALL module
	// state here, CPU and GPU. OnUnload fires right before the scene is destroyed,
	// also GPU-idle; module destructors release remaining ComPtrs.
	virtual void OnLoad(MCEngine&, MCScene&) {}
	virtual void OnUnload(MCEngine&, MCScene&) {}
	virtual void OnImGui(MCEngine&, MCScene&) {}

	virtual void ToJson(nlohmann::json& j) const = 0;
	virtual void FromJson(const nlohmann::json& j) = 0;
};