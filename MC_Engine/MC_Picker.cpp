#include "MC_Picker.h"
#include "MCScene.h"
// naive vanilla approach of using CPU only
// converts screen space position (x,y) to view space coordinate at z = near plane of camera
void MC_Picker::ScreenSpaceToCamViewSpace(XMFLOAT2 s_xy) {
	float v_nearZ = m_camera.GetNearZ();
	std::vector<float> wh = m_Engine.GetScreenSize();
	float w = wh[0];
	float h = wh[1];
	float P00 = m_camera.GetProj4x4f()._11;
	float P11 = m_camera.GetProj4x4f()._22;
	float v_x = (2.0f * s_xy.x / w - 1.0f) / P00 * v_nearZ;
	float v_y = (1.0f - 2.0f * s_xy.y / h) / P11 * v_nearZ;
	v_rayDirection = { v_x, v_y, v_nearZ, 0.0f };
}

float MC_Picker::TestRayBBHit(RenderItem& ri) {
	// World-space ray vs world-space AABB — handles non-uniform scale correctly.
	XMMATRIX world = XMLoadFloat4x4(&ri.World);

	XMMATRIX view = m_camera.GetView();
	XMVECTOR detView = XMMatrixDeterminant(view);
	XMMATRIX invView = XMMatrixInverse(&detView, view);

	// Transform ray direction from view space to world space
	XMVECTOR ray_dirW = XMVector3TransformNormal(v_rayDirection, invView);
	ray_dirW = XMVector3Normalize(ray_dirW);

	XMVECTOR ray_origW = m_camera.GetPosition();

	// Transform local AABB to world space (handles non-uniform scale via corner hull)
	DirectX::BoundingBox worldBounds;
	ri.Bounds.Transform(worldBounds, world);

	float dist = 0.0f;
	if (worldBounds.Intersects(ray_origW, ray_dirW, dist)) {
		XMVECTOR hitPointW = XMVectorMultiplyAdd(ray_dirW, XMVectorReplicate(dist), ray_origW);

		XMFLOAT3 hitPos;
		XMStoreFloat3(&hitPos, hitPointW);

		std::cout << "Hit: " << ri.Name
			<< " | Pos (world): "
			<< hitPos.x << ", "
			<< hitPos.y << ", "
			<< hitPos.z << std::endl;

		return dist;
	}
	return -1.0f;
}

float MC_Picker::TestRayVertexHit(RenderItem& ri) {
	// choose 3 vertices in ri that are closest to the ray hit point.
	return 0.0f;
}

int MC_Picker::PickRenderItemOnScreen(XMFLOAT2 xy, MCScene& scene) {
	ScreenSpaceToCamViewSpace(xy);
	float minDist = m_camera.GetFarZ();
	int selected_idx = -1;
	int tested = 0;
	for (int i = 0; i < static_cast<int>(scene.allRitems.size()); ++i) {
		auto& ri = scene.allRitems[i];
		if (!ri->insideFrustrum)
			continue;
		tested++;
		float ray_hitDist = TestRayBBHit(*ri);
		// std::cout << "hit: " << ri->Name << ", dist : " << ray_hitDist << std::endl;
		if (ray_hitDist < m_camera.GetNearZ() || minDist < ray_hitDist)
			continue;
		minDist = ray_hitDist;
		selected_idx = i;
	}
	std::cout << "tested items in frustrum : " << tested << std::endl;
	if (selected_idx >= 0) {
		const auto& hit = scene.allRitems[selected_idx];
		std::cout << "NAME:" << hit->Name << " | allRitems index : " << selected_idx
			<< " | objCBindex : " << hit->ObjCBIndex << std::endl;
	} else {
		std::cout << "no hit" << std::endl;
	}
	std::cout << "----------------------" << std::endl;
	return selected_idx;
}