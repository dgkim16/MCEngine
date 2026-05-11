#pragma once
#include "MCEngine.h"
#include "Camera.h"

class MCScene;
class MC_Picker {
public:
	MC_Picker(const MC_Picker& rhs) = delete;
	MC_Picker& operator=(const MC_Picker& rhs) = delete;
	static MC_Picker& GetPicker(MCEngine& engine) {
		static MC_Picker singleton(engine);
		return singleton;
	}
	// Returns the index of the picked render item in scene.allRitems, or -1 on miss.
	// Sentinel matches MCEngine::mSelectedItemIndex's convention.
	int PickRenderItemOnScreen(XMFLOAT2 xy, MCScene& scene);
	
private:
	explicit MC_Picker(MCEngine& engine)
		: m_Engine(engine),
		  m_camera(engine.GetMainCamera()),
		  v_rayDirection(XMVectorZero()) 
	{}

private:
	MCEngine& m_Engine;
	Camera& m_camera;
	XMVECTOR v_rayDirection;

	void ScreenSpaceToCamViewSpace(XMFLOAT2 s_xy); // 's_' stands for 'screen_'
	float TestRayBBHit(RenderItem& ri);
	float TestRayVertexHit(RenderItem& ri);
};