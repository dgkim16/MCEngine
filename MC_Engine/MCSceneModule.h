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

	virtual void OnLoad(MCEngine&, MCScene&) {}
	virtual void OnActivate(MCEngine&, MCScene&) {}
	virtual void OnDeactivate(MCEngine&, MCScene&) {}
	virtual void OnUpdate(MCEngine&, MCScene&, float /*dt*/) {}
	// OnDraw fires from inside MCEngine::ForwardPass at a defined-binding-state point
	// (compute root signature + CBV/SRV/UAV heap bound by surrounding engine code).
	// Modules record GPU dispatches here, NOT in OnUpdate. Future will split into
	// multiple OnDraw_* hooks (pre-opaque, post-opaque, post-transparent, etc.) when
	// modules need different frame phases.
	virtual void OnDraw(MCEngine&, MCScene&) {}
	virtual void OnImGui(MCEngine&, MCScene&) {}

	virtual void ToJson(nlohmann::json& j) const = 0;
	virtual void FromJson(const nlohmann::json& j) = 0;
};