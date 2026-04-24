#pragma once
#include "Scene.h"

// Minimal scene: a flat grid and a box, used to demonstrate scene switching.
class Scene_Empty : public Scene
{
public:
	Scene_Empty() { name = "Empty"; }
	void Load(MCEngine& engine) override;
	void Activate(MCEngine& engine) override;
};
