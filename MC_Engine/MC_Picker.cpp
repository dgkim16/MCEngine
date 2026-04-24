#include "MC_Picker.h"

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

RenderItem& MC_Picker::PickRenderItem(XMFLOAT2 xy, std::vector<std::unique_ptr<RenderItem>>& mAllRitems) {
	ScreenSpaceToCamViewSpace(xy);
	float minDist = m_camera.GetFarZ();
	RenderItem selected_ri;
	int tested = 0;
	for (auto& ri : mAllRitems) {
		if (!ri->insideFrustrum)
			continue;
		tested++;
		float ray_hitDist = TestRayBBHit(*ri);
		// std::cout << "hit: " << ri->Name << ", dist : " << ray_hitDist << std::endl;
		if (ray_hitDist < m_camera.GetNearZ() || minDist < ray_hitDist)
			continue;
		minDist = ray_hitDist;
		selected_ri = *ri;
	}
	std::cout << "tested items in frustrum : " << tested << std::endl;
	std::cout << "NAME:" << selected_ri.Name  << " | objCBindex : " << selected_ri.ObjCBIndex << std::endl;
	std::cout << "----------------------" << std::endl;
	return selected_ri;
}