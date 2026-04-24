#pragma once
#include "Scene.h"

class Scene_Ch7 : public Scene
{
public:
	Scene_Ch7() { name = "Ch7"; }
	void Load(MCEngine& engine) override;
	void Activate(MCEngine& engine) override;

private:
	void BuildGeometry(MCEngine& engine);
	void BuildSpriteGeometry(MCEngine& engine);
	void BuildMaterials(MCEngine& engine);
	void BuildRenderItems(MCEngine& engine);

	// stored so Activate() can re-apply them on scene re-entry
	RenderItem* mTessellatedRitem = nullptr;
	RenderItem* mModelRitem = nullptr;
	RenderItem* mReflectedModelRitem = nullptr;
};
