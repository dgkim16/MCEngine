#include "Legacy/MCLight.h"
#include "Renderer/D3D12/DescHeapManager.h"
#include "Renderer/D3D12/BarrierManager.h"

namespace {
	// plane dot(n, p) + d = 0; "inside" is dot(n, p) + d >= 0
	struct ClipPlane { DirectX::XMFLOAT3 n; float d; };
	using ClipPoly = std::vector<DirectX::XMFLOAT3>;

	float PlaneDist(const ClipPlane& pl, const DirectX::XMFLOAT3& p) { return pl.n.x * p.x + pl.n.y * p.y + pl.n.z * p.z + pl.d; }

	// Sutherland-Hodgman: the part of a convex polygon on the inside of one plane
	ClipPoly ClipPolygon(const ClipPoly& poly, const ClipPlane& pl) {
		ClipPoly out;
		for (size_t k = 0; k < poly.size(); ++k) {
			const DirectX::XMFLOAT3& a = poly[k];
			const DirectX::XMFLOAT3& b = poly[(k + 1) % poly.size()];
			const float da = PlaneDist(pl, a), db = PlaneDist(pl, b);
			if (da >= 0.0f) out.push_back(a);
			if ((da >= 0.0f) != (db >= 0.0f)) { // edge crosses the plane: keep the crossing point
				const float t = da / (da - db);
				out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t });
			}
		}
		return out;
	}

	// inward-facing planes of a convex hexahedron (8 corners, 6 quad faces listed as corner cycles)
	void HexPlanes(const DirectX::XMFLOAT3 v[8], const int faces[6][4], ClipPlane out[6]) {
		using namespace DirectX;
		XMVECTOR centroid = XMVectorZero();
		for (int k = 0; k < 8; ++k) centroid = XMVectorAdd(centroid, XMLoadFloat3(&v[k]));
		centroid = XMVectorScale(centroid, 1.0f / 8.0f);
		for (int f = 0; f < 6; ++f) {
			const XMVECTOR a = XMLoadFloat3(&v[faces[f][0]]);
			XMVECTOR n = XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&v[faces[f][1]]), a), XMVectorSubtract(XMLoadFloat3(&v[faces[f][2]]), a));
			if (XMVectorGetX(XMVector3Dot(n, XMVectorSubtract(centroid, a))) < 0.0f) n = XMVectorNegate(n);
			XMStoreFloat3(&out[f].n, n);
			out[f].d = -XMVectorGetX(XMVector3Dot(n, a));
		}
	}

	// corners of (view frustum ∩ box): frustum faces clipped by the box, plus box faces clipped by the frustum.
	// frustum corner order matches ndcCorners in UpdateShadowMapPassCBs (far TL, TR, BL, BR, near TL, TR, BL, BR). Empty if disjoint
	std::vector<DirectX::XMFLOAT3> ClipFrustumToBox(const DirectX::XMVECTOR frustumCorners[8], const DirectX::BoundingBox& box) {
		// frustumFaces indexes worldCorners (same ordering as ndcCoreners : 
		// 0, 1, 2, 3	far: top-left, top-right, bottom-left, bottom-right
		// 4, 5, 6, 7	near: top - left, top - right, bottom - left, bottom - right)
		static const int frustumFaces[6][4] = { {0,1,3,2}, {4,5,7,6}, {0,1,5,4}, {2,3,7,6}, {0,2,6,4}, {1,3,7,5} };
		// boxFaces indexes box corners built by bit tests on index. Bit 0b_0000_0ZYX, 0 = min, 1 = max
		static const int boxFaces[6][4] = { {0,2,6,4}, {1,3,7,5}, {0,1,5,4}, {2,3,7,6}, {0,1,3,2}, {4,5,7,6} };
		DirectX::XMFLOAT3 fr[8], bx[8];
		for (int k = 0; k < 8; ++k) DirectX::XMStoreFloat3(&fr[k], frustumCorners[k]);
		const float pad = 0.05f; // keeps receivers lying exactly on the bounds, and flat bounds, from being clipped away
		const DirectX::XMFLOAT3 lo{ box.Center.x - box.Extents.x - pad, box.Center.y - box.Extents.y - pad, box.Center.z - box.Extents.z - pad };
		const DirectX::XMFLOAT3 hi{ box.Center.x + box.Extents.x + pad, box.Center.y + box.Extents.y + pad, box.Center.z + box.Extents.z + pad };
		//  Bit 0b_0000_0ZYX, 0 = min, 1 = max
		for (int k = 0; k < 8; ++k) bx[k] = { (k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z };
		ClipPlane frPlanes[6], bxPlanes[6];
		HexPlanes(fr, frustumFaces, frPlanes);
		HexPlanes(bx, boxFaces, bxPlanes);

		std::vector<DirectX::XMFLOAT3> points;
		auto clipFacesInto = [&](const DirectX::XMFLOAT3 v[8], const int faces[6][4], const ClipPlane planes[6]) {
			for (int f = 0; f < 6; ++f) {
				ClipPoly poly{ v[faces[f][0]], v[faces[f][1]], v[faces[f][2]], v[faces[f][3]] };
				for (int p = 0; p < 6 && !poly.empty(); ++p) poly = ClipPolygon(poly, planes[p]);
				points.insert(points.end(), poly.begin(), poly.end());
			}
		};
		clipFacesInto(fr, frustumFaces, bxPlanes); // step 1: frustum faces against the box
		clipFacesInto(bx, boxFaces, frPlanes);     // step 2: box faces against the frustum
		return points;                             // step 3: together, the corners of the intersection
	}
}

void MCLight::Initialize(ShadowMapVolumeType volumeType) {
	if (!mData.mCastShadow)
		return;
	if (volumeType == ShadowMapVolumeType::NOT_SHADOW_CASTER)
		mShadowMapVolumeType = ShadowMapVolumeType:: PERSP_VIEW; //ORTHO_LIGHT;//
	else
		mShadowMapVolumeType = volumeType;
}

void MCLightManager::MarkAllLightsDirty(int dirty) {
	for (auto& l : mLights)
		l.mIsDirty = dirty;
}

void MCLightManager::SetupShadowMapDSVSRVs() {
	auto& dhm = DescHeapManager::Get();

	for (auto& mSM : mLights) {
		if (!mSM.mShadowMap) continue;
		dhm.QueueRemoval_Texture(*(mSM.mShadowMap));
	}
	dhm.FlushPending();
	for (int i = 0; i < mLights.size(); ++i) {
		if (!mLights[i].mData.mCastShadow) continue;
		dhm.CreateDsv(*(mLights[i].mShadowMap), D3D12_DSV_FLAG_NONE, mDepthShadowMapDSVFormat, false, 0);
		dhm.CreateSrv2d(*(mLights[i].mShadowMap), mDepthShadowMapSRVFormat, false, MC_VIEW_TIER_STATIC);
	}
}

void MCLightManager::UpdateShadowMapPassCBs(FrameResource& currFrameResource) {
	auto& curPassCB = currFrameResource.PassCB;
	if (mLights.empty()) return;
	// compute (projection view) matrix
	for (int i = 0; i < mLights.size(); ++i) {
		if (mLights[i].mData.mCastShadow == 0) continue; // goto next iter // j = i is guaranteed. splitting i and j for clarity tho
		// if (mLights[i].mIsDirty <= 0) continue;
		if (mLights[i].GetShadowMapVolumeType() == ShadowMapVolumeType::COUNT || 
			mLights[i].GetShadowMapVolumeType() == ShadowMapVolumeType::NOT_SHADOW_CASTER) continue; // shouldn't happen, but can debug here.

		// XMMATRIX cannot be declared constexpr - constructors and operations rely on compiler intrinsics (HW acceleration-SIMD/SSE/AVX)
		static const DirectX::XMMATRIX toTexture(  
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f); // bias is applied in world space instead (worldBias below)

		PerPassCB shadowCB{};
		
		
		DirectX::XMFLOAT3 mPosition = mLights[i].mData.mLightPosition;
		DirectX::XMFLOAT3 mDirection = mLights[i].mData.mLightDirection;
		DirectX::XMFLOAT3 mWorldUp = { 0.0f,1.0f,0.0f };
		DirectX::XMVECTOR P = XMLoadFloat3(&mPosition);
		DirectX::XMVECTOR L = XMLoadFloat3(&mDirection); 
		DirectX::XMVECTOR U = XMLoadFloat3(&mWorldUp);
		const float d = DirectX::XMVectorGetX(DirectX::XMVector3Dot(L, U));
		if (d > 0.99f || d < -0.99f)
			U = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
		DirectX::XMMATRIX lightView = DirectX::XMMatrixLookToLH(P, DirectX::XMVectorNegate(L), U); // computes u,v,w and outputs the view matrix
		// world-space receiver bias: move lookup positions GetDepthBias() world units toward the light (L points toward the light),
		// so the bias is the same distance under any shadow projection (ortho, PSM, LiSPSM warp). Applied to mShadowTransform only, not the shadow pass
		const DirectX::XMMATRIX worldBias = DirectX::XMMatrixTranslationFromVector(DirectX::XMVectorScale(DirectX::XMVector3Normalize(L), mLights[i].GetDepthBias()));
		if (mLights[i].GetShadowMapVolumeType() == ShadowMapVolumeType::ORTHO_LIGHT) {
			if (mLights[i].mData.mLightType == 0) {
				// Bounding sphere for directional light (not AABB bounding box) to get cube bounds
				// (frustrum-like - rotational orientation follows light coordin.syst, but position of frustrum is centered / affected by scene center, not light position)
				// sphere used instead of box because regardless of which coordinate system, +- radius from center along that coordinate system's xyz axis
				// guarantees the left/right/top/bottom walls (cube)
				DirectX::XMFLOAT3 mCenterPos = { 0.0f,0.0f,0.0f };
				float radius = mLights[i].GetShadowMapFrustum(); // shadow quality. world units per shadow texel = 2r / mapSize. CSM (cascaded shadow maps) use several maps instead (do later)
				DirectX::XMVECTOR simd_cls = XMVector3TransformCoord(XMLoadFloat3(&mCenterPos), lightView); // transform center of sphere to light space

				// now convert sphere into box (ortho projection)
				DirectX::XMFLOAT3 f_cls;
				XMStoreFloat3(&f_cls, simd_cls);
				float l_wall = f_cls.x - radius; // left
				float r_wall = f_cls.x + radius; // right
				float t_wall = f_cls.y + radius; // top
				float b_wall = f_cls.y - radius; // bottom
				float n_wall = f_cls.z - radius; // near
				float f_wall = f_cls.z + radius; // far
				DirectX::XMMATRIX mLightProj = DirectX::XMMatrixOrthographicOffCenterLH(l_wall, r_wall, b_wall, t_wall, n_wall, f_wall); // = NDC, as perp.divide on ortho is div by 1
				DirectX::XMMATRIX mLightViewProj = XMMatrixMultiply(lightView, mLightProj);

				XMStoreFloat4x4(&shadowCB.gViewProj, XMMatrixTranspose(mLightViewProj));
				curPassCB->CopyData(i + 1, shadowCB);
				
				auto mShadowTransform = XMMatrixMultiply(worldBias, XMMatrixMultiply(mLightViewProj, toTexture));
				XMStoreFloat4x4(&mLights[i].mData.mShadowTransform, XMMatrixTranspose(mShadowTransform));
			}
		}
		else if (mLights[i].GetShadowMapVolumeType() == ShadowMapVolumeType::PERSP_VIEW) {
			bool isPSM = false;
			if (isPSM) {	// PSM path, kept for comparison. I do LiSPSM, not this
				using namespace DirectX;
				const auto& camCB = currFrameResource.mCachedMainPassCB;
				const XMMATRIX camView = XMMatrixTranspose(XMLoadFloat4x4(&camCB.gView));
				const XMMATRIX camProj = XMMatrixTranspose(XMLoadFloat4x4(&camCB.gProj));

				// Tunables
				const float kVirtualShift = 5.0f;  // world units the virtual camera is pulled back; also weakens the warp (acts as the pseudo near plane)
				const float kDirEps = 0.01f; // |w| below this: the light stays directional in NDC space
				const float kNearPull = 0.1f;  // light in front: pull the light's near plane toward it to keep casters beyond the shadow distance
				const float kDirExtend = 10.0f; // directional-in-NDC case: extend the depth range toward the light (NDC units)

				// 1. Virtual camera: same angular FOV as the real one, eye moved back by kVirtualShift.
				//    Its far plane is the shadow distance, so its NDC cube is exactly the shadowed region.
				const float xScale = XMVectorGetX(camProj.r[0]);
				const float yScale = XMVectorGetY(camProj.r[1]);
				const float vNear = kVirtualShift + camCB.gNearZ;                              // = the real camera's near plane
				const float vFar = kVirtualShift + (float)mLights[i].GetShadowMapFrustum();
				const XMMATRIX virtView = XMMatrixMultiply(camView, XMMatrixTranslation(0.0f, 0.0f, kVirtualShift));
				const XMMATRIX virtProj = XMMatrixPerspectiveLH(2.0f * vNear / xScale, 2.0f * vNear / yScale, vNear, vFar);
				const XMMATRIX camVP = XMMatrixMultiply(virtView, virtProj);

				// 2. Region to cover in NDC: x,y in [-1,1], z in [zBehind,1].
				//    zBehind is the NDC depth at half the virtual near distance, so casters up to ~kVirtualShift/2 behind the camera are kept.
				const float A = vFar / (vFar - vNear);  // NDC z of a point at infinity
				const float zBehind = -A;
				XMVECTOR box[8];
				for (int c = 0; c < 8; ++c)
					box[c] = XMVectorSet((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : zBehind, 1.0f);
				const XMVECTOR boxCenter = XMVectorSet(0.0f, 0.0f, 0.5f * (zBehind + 1.0f), 1.0f);

				auto pickUp = [](FXMVECTOR dir) {
					const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
					const float d = XMVectorGetX(XMVector3Dot(dir, up));
					return (d > 0.99f || d < -0.99f) ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f) : up;
					};

				// 3. Transform the light into NDC space. mLightDirection points toward the light.
				const XMVECTOR toLight = XMVectorSetW(XMVector3Normalize(XMLoadFloat3(&mLights[i].mData.mLightDirection)), 0.0f);
				const XMVECTOR lightH = XMVector4Transform(toLight, camVP);
				const float lw = XMVectorGetW(lightH);

				XMMATRIX ppsView, ppsProj;
				if (lw > -kDirEps && lw < kDirEps) {
					// Light perpendicular to the view axis: stays directional in NDC -> orthographic.
					const XMVECTOR lookDir = XMVector3Normalize(XMVectorNegate(XMVectorSetW(lightH, 0.0f)));
					ppsView = XMMatrixLookToLH(boxCenter, lookDir, pickUp(lookDir));
					XMVECTOR mn = XMVectorSplatInfinity(), mx = XMVectorNegate(mn);
					for (int c = 0; c < 8; ++c) {
						const XMVECTOR p = XMVector3TransformCoord(box[c], ppsView);
						mn = XMVectorMin(mn, p); mx = XMVectorMax(mx, p);
					}
					XMFLOAT3 lo, hi; XMStoreFloat3(&lo, mn); XMStoreFloat3(&hi, mx);
					ppsProj = XMMatrixOrthographicOffCenterLH(lo.x, hi.x, lo.y, hi.y, lo.z - kDirExtend, hi.z);
				}
				else {
					// Light becomes a point light at lightH / w. It always lands at NDC z = A > 1, outside the box.
					const XMVECTOR lightPos = XMVectorScale(lightH, 1.0f / lw);
					XMVECTOR lookDir = XMVector3Normalize(XMVectorSubtract(boxCenter, lightPos));
					ppsView = XMMatrixLookToLH(lightPos, lookDir, pickUp(lookDir));
					float minZ = 1e30f;
					for (int c = 0; c < 8; ++c)
						minZ = (std::min)(minZ, XMVectorGetZ(XMVector3TransformCoord(box[c], ppsView)));
					if (minZ < 1e-4f) {
						// A corner is beside or behind the light: look straight down -z instead.
						// The box lies at z <= 1 < A, so every corner is in front.
						lookDir = XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f);
						ppsView = XMMatrixLookToLH(lightPos, lookDir, pickUp(lookDir));
					}
					float l = 1e30f, r = -1e30f, b = 1e30f, t = -1e30f, zMin = 1e30f, zMax = -1e30f;
					for (int c = 0; c < 8; ++c) {
						XMFLOAT3 p; XMStoreFloat3(&p, XMVector3TransformCoord(box[c], ppsView));
						l = (std::min)(l, p.x / p.z); r = (std::max)(r, p.x / p.z);
						b = (std::min)(b, p.y / p.z); t = (std::max)(t, p.y / p.z);
						zMin = (std::min)(zMin, p.z); zMax = (std::max)(zMax, p.z);
					}
					if (lw > 0.0f) {
						// Light in front of the camera: normal depth order.
						const float nz = zMin * kNearPull;
						ppsProj = XMMatrixPerspectiveOffCenterLH(l * nz, r * nz, b * nz, t * nz, nz, zMax);
					}
					else {
						// Light behind the camera: in NDC it sits "past infinity", so the point FARTHEST from lightPos
						// is the one closest to the real light. Swapping near and far reverses depth so the
						// shadow pass (DepthFunc LESS) still keeps the true occluder.
						ppsProj = XMMatrixPerspectiveOffCenterLH(l * zMax, r * zMax, b * zMax, t * zMax, zMax, zMin);
					}
				}

				// 4. world -> virtual camera NDC -> light view in NDC -> light projection in NDC
				const XMMATRIX shadowVP = XMMatrixMultiply(XMMatrixMultiply(camVP, ppsView), ppsProj);
				XMStoreFloat4x4(&shadowCB.gViewProj, XMMatrixTranspose(shadowVP));
				curPassCB->CopyData(i + 1, shadowCB);
				XMStoreFloat4x4(&mLights[i].mData.mShadowTransform, XMMatrixTranspose(XMMatrixMultiply(worldBias, XMMatrixMultiply(shadowVP, toTexture))));
				return;
			}

			// ==================================================
			// LiSPSM
			// 0. instead of using a sphere to represent the shadow map frustum volume, 
			// use the main camera's frustum volume as is. HOW :
			// 0-1. convert NDC coordinates of main camera frustum's 8 points (x, y ∈ {−1, +1}, and for D3D, z ∈ {0, 1}) back into world space
			// 0-2. convert from world space into light space
			// 0-3. convert from light space to projection space + get shadowTransform also
			const auto& cached = currFrameResource.mCachedMainPassCB;
			const DirectX::XMMATRIX invCamViewProj = XMMatrixTranspose(XMLoadFloat4x4(&cached.gInvViewProj));
			const DirectX::XMFLOAT3 ndcCorners[8] = {
				{ -1.0f,  1.0f, 1.0f }, // far  top    left
				{  1.0f,  1.0f, 1.0f }, // far  top    right
				{ -1.0f, -1.0f, 1.0f }, // far  bottom left
				{  1.0f, -1.0f, 1.0f }, // far  bottom right
				{ -1.0f,  1.0f, 0.0f }, // near top    left
				{  1.0f,  1.0f, 0.0f }, // near top    right
				{ -1.0f, -1.0f, 0.0f }, // near bottom left
				{  1.0f, -1.0f, 0.0f }, // near bottom right
			};
			DirectX::XMVECTOR worldCorner[8];
			auto vMin = DirectX::XMVectorSplatInfinity(); // infinity
			auto vMax = DirectX::XMVectorNegate(vMin);
			for (int c = 0; c < 8; ++c) {
				worldCorner[c] = XMVector3TransformCoord(XMLoadFloat3(&ndcCorners[c]), invCamViewProj); // vector matrix mult
			}
			const float maxShadowDist = mLights[i].GetShadowMapFrustum();
			const float s = std::clamp((maxShadowDist - cached.gNearZ) / (cached.gFarZ - cached.gNearZ), 0.0f, 1.0f);
			for (int c = 0; c < 4; ++c)
				worldCorner[c] = DirectX::XMVectorLerp(worldCorner[c + 4], worldCorner[c], s); // use a closer far plane. xyz all scaled.

			// 0-4. clip the view frustum to the scene bounds, so the fit covers only where geometry exists.
			// fitPoints are the corners of (frustum ∩ scene box); without clipping, the 8 frustum corners
			std::vector<DirectX::XMFLOAT3> fitPoints;
			if (mHasSceneBounds && mLights[i].GetClipToScene())
				fitPoints = ClipFrustumToBox(worldCorner, mSceneBounds);
			if (fitPoints.empty()) { // clipping off, no bounds yet, or the view misses the scene
				fitPoints.resize(8);
				for (int c = 0; c < 8; ++c) DirectX::XMStoreFloat3(&fitPoints[c], worldCorner[c]);
			}
			for (auto& p : fitPoints) // world -> light view space
				DirectX::XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&p), lightView));


			// 1. define Warp space axis and origin
			// 1-A. Compute axis
			DirectX::XMVECTOR camWorldPos = XMLoadFloat3(&cached.gEyePosW);
			DirectX::XMVECTOR camLightPos = XMVector3TransformCoord(camWorldPos, lightView);
			DirectX::XMFLOAT3 camWorldForward{ cached.gView.m[2][0], cached.gView.m[2][1], cached.gView.m[2][2] }; // _31, _32, _33 
			DirectX::XMVECTOR camLightForward = XMVector3TransformNormal(XMLoadFloat3(&camWorldForward), lightView); // use TransformNormal() to discard translation
			DirectX::XMFLOAT3 warpUpFloat3 { 0.0f,0.0f,1.0f };

			DirectX::XMVECTOR warpUp = XMLoadFloat3(&warpUpFloat3);
			DirectX::XMVECTOR warpFw = DirectX::XMVectorSubtract(camLightForward, DirectX::XMVectorMultiply(warpUp, DirectX::XMVector3Dot(camLightForward, warpUp)));

			// 1-AB. Check if camViewDir is near parallel or anti-parallel to light Dir. Use fallback if so.
			DirectX::XMFLOAT3 warpFwLengthSq;
			auto warpFewLengthSqXMV = DirectX::XMVector3LengthSq(warpFw);
			DirectX::XMStoreFloat3(&warpFwLengthSq, warpFewLengthSqXMV);
			float warpFwLength = sqrt(warpFwLengthSq.x);
			if (warpFwLength > 0.01f) {
				// 1-B. find how far each frustum points are along the warpFw direction from the light origin
				warpFw = DirectX::XMVector3Normalize(warpFw);
				auto warpsMin = DirectX::XMVectorSplatInfinity(); // infinity
				auto warpsMax = DirectX::XMVectorNegate(warpsMin);
				for (const auto& p : fitPoints) {
					auto curr = DirectX::XMVector3Dot(warpFw, XMLoadFloat3(&p));
					warpsMin = DirectX::XMVectorMin(curr, warpsMin);
					warpsMax = DirectX::XMVectorMax(curr, warpsMax);
				}
				DirectX::XMFLOAT3 sMax, sMin;
				DirectX::XMStoreFloat3(&sMin, warpsMin);
				DirectX::XMStoreFloat3(&sMax, warpsMax);
				auto warpECamPos = DirectX::XMVector3Dot(warpFw, camLightPos);

				// 1-C. Compute origin 
				// E = camPos_ls - (s_cam - s_min + n) * axis
				// n_opt = (z_n + sqrt(z_n * z_f)) / sin γ .
				// float warpAmount = 0.7f / warpFwLength; // ========================= THIS IS A HYPERPARAMETER
				// pseudo near plane: n_opt balances error between z_n and z_f, so z_n must be where receivers start, not a 0.01 camera near plane
				const float zn = (std::max)(cached.gNearZ, mLights[i].GetPseudoNear());
				const float zf = maxShadowDist;
				float warpAmount;
				if (mLights[i].GetWarpAmount() > 0.01f) {
					warpAmount = mLights[i].GetWarpAmount();
				}
				else
					warpAmount = (zn + sqrt(zn * zf)) / warpFwLength;
				currentWarp = warpAmount;
				// 1-D. fade the warp out as the camera turns toward (anti)parallel with the light (OGRE LiSPSM):
				// past the threshold, n grows smoothly up to 21x, so switching to the fallback below 0.01 causes no visible pop.
				// |cos γ| = sqrt(1 - sin²γ), and warpFwLength is sin γ (camLightForward is unit length)
				const float cosCamLight = sqrt((std::max)(0.0f, 1.0f - warpFwLength * warpFwLength));
				const float cosCamLightThreshold = 0.9f; // ~26 degrees from parallel
				if (cosCamLight > cosCamLightThreshold)
					warpAmount *= 1.0f + 20.0f * ((cosCamLight - cosCamLightThreshold) / (1.0f - cosCamLightThreshold));
				DirectX::XMFLOAT3 warpHyperParam{ warpAmount ,warpAmount ,warpAmount };
				auto warpN = DirectX::XMVectorAdd(DirectX::XMVectorSubtract(warpECamPos, warpsMin), DirectX::XMLoadFloat3(&warpHyperParam));
				DirectX::XMVECTOR warpE = DirectX::XMVectorSubtract(camLightPos, DirectX::XMVectorMultiply(warpFw, warpN));

				// 2. Construct warp view and warp projection matrix
				auto warpViewMatrix = DirectX::XMMatrixLookToLH(warpE, warpFw, warpUp); //cam pos, facing dir, up dir
				// squeezing also is affected by warp amount
				auto warpProjMatrix = DirectX::XMMatrixPerspectiveOffCenterLH(-warpAmount, warpAmount, -warpAmount, warpAmount, warpAmount, warpAmount + (sMax.x - sMin.x));
				auto warpViewProjMatrix = DirectX::XMMatrixMultiply(warpViewMatrix, warpProjMatrix);

				// 3. Construct Rotation matrix - 90 degrees about X. GPU stores clip-space depth as z, and we want shadow map to use depth along the direction of light.
				// currently the light is y axis, not z axis in the warp view space. Rotating by 90 on x maps (x, y, z) to (x, -z, y)
				// we rotate, and not swap. swapping results in mirrored image, 
				// which messes up triangle winding -> back-face culling (shadow pass) results in removing wrong faces
				auto RotX90 = DirectX::XMMatrixRotationX(DirectX::XM_PIDIV2);
				auto warpViewProjRot = DirectX::XMMatrixMultiply(warpViewProjMatrix, RotX90);

				// 4. convert camera frustum edges into warpViewProjMatrix + rotation

				for (const auto& p : fitPoints) {
					DirectX::XMVECTOR warpNDC = DirectX::XMVector3TransformCoord(XMLoadFloat3(&p), warpViewProjMatrix);
					warpNDC = DirectX::XMVectorDivide(warpNDC, DirectX::XMVectorSplatW(warpNDC)); // this does homogenous divide
					warpNDC = DirectX::XMVector3TransformCoord(warpNDC, RotX90);
					vMin = DirectX::XMVectorMin(warpNDC, vMin);
					vMax = DirectX::XMVectorMax(warpNDC, vMax);
				}
				// now camera's frustum corners are all represented in warp view space
				DirectX::XMFLOAT3 fMax, fMin;
				DirectX::XMStoreFloat3(&fMax, vMax);
				DirectX::XMStoreFloat3(&fMin, vMin);
				float t = fMax.y; float b = fMin.y;
				float r = fMax.x; float l = fMin.x;
				float f = fMax.z;
				float n = fMin.z; // body B already reaches the most light-ward caster
				const auto shadowProj = DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
				const auto lightViewWarpViewProjRot = XMMatrixMultiply(lightView, warpViewProjRot);
				const auto lightViewWarpViewProjRotshadowProj = XMMatrixMultiply(lightViewWarpViewProjRot, shadowProj);


				XMStoreFloat4x4(&shadowCB.gViewProj, XMMatrixTranspose(lightViewWarpViewProjRotshadowProj));
				curPassCB->CopyData(i + 1, shadowCB);
				auto mShadowTransform = XMMatrixMultiply(worldBias, XMMatrixMultiply(lightViewWarpViewProjRotshadowProj, toTexture));
				XMStoreFloat4x4(&mLights[i].mData.mShadowTransform, XMMatrixTranspose(mShadowTransform));

			}
			// 1-AB. fallback - light dir is (anti) parallel to camera dir
			else {
				for (const auto& p : fitPoints) {
					vMin = DirectX::XMVectorMin(XMLoadFloat3(&p), vMin);
					vMax = DirectX::XMVectorMax(XMLoadFloat3(&p), vMax);
				}
				// now camera's frustum corners are all represented in light view space
				DirectX::XMFLOAT3 fMax, fMin;
				DirectX::XMStoreFloat3(&fMax, vMax);
				DirectX::XMStoreFloat3(&fMin, vMin);
				float t = fMax.y; float b = fMin.y;
				float r = fMax.x; float l = fMin.x;
				float f = fMax.z; float n = fMin.z; // bias
				const auto lightProj = DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
				const auto lightViewProj = XMMatrixMultiply(lightView, lightProj);

				XMStoreFloat4x4(&shadowCB.gViewProj, XMMatrixTranspose(lightViewProj));
				curPassCB->CopyData(i + 1, shadowCB);
				auto mShadowTransform = XMMatrixMultiply(worldBias, XMMatrixMultiply(lightViewProj, toTexture));
				XMStoreFloat4x4(&mLights[i].mData.mShadowTransform, XMMatrixTranspose(mShadowTransform));
			}
		}
	} // end of for loop of all lights
}

void MCLightManager::UpdateLightsDataSRVs(FrameResource& currFrameResource) {
	auto currLightCB = currFrameResource.LightCB.get();
	// no-opt greedy solution : regardless of dirty, updates all lights for now
	for (int i = 0; i < mLights.size(); ++i) {
		// if (mLights[i].mIsDirty <= 0) continue;
		if (mLights[i].mData.mCastShadow) {
			mLights[i].mData.mShadowMapIndex = mLights[i].mShadowMap->SRVs[0].offset;
		}
		currLightCB->CopyData(i, mLights[i].mData);
		// --mLights[i].mIsDirty;
	}

}


// MUST CALL FlushCommandQueue() before calling this function!
void MCLightManager::CreateShadowMapResources(ID3D12Device* md3dDevice, BarrierManager& barriers) {
	// COMMAND QUEUE MUST HAVE BEEN FLUSHED BEFORE CALLING THIS FUNCTION (FlushCommandQueue)
	auto& dhm = DescHeapManager::Get();
	for (auto& mSM : mLights) {
		if (!mSM.mShadowMap) continue;
		dhm.QueueRemoval_Texture(*(mSM.mShadowMap));
		mSM.mShadowMap.reset();
	}
	dhm.FlushPending();
	
	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	// create shadow map resources
	D3D12_RESOURCE_DESC shadowMapDesc = {};
	shadowMapDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	shadowMapDesc.Alignment = 0;
	
	shadowMapDesc.DepthOrArraySize = 1;
	shadowMapDesc.MipLevels = 1; // only need 1 for D/SBV

	shadowMapDesc.Format = GetDepthShadowMapFormat(); //mDepthStencilFormat;
	shadowMapDesc.SampleDesc.Count = 1;
	shadowMapDesc.SampleDesc.Quality = 0;
	shadowMapDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	shadowMapDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthShadowMapDSVFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	for (int i = 0; i < mLights.size(); ++i) {
		shadowMapDesc.Width = (UINT64)mLights[i].GetShadowMapResolution();
		shadowMapDesc.Height = (UINT64)mLights[i].GetShadowMapResolution();
		MCTextureResource shadowMap = {};
		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&shadowMapDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&optClear,  
			IID_PPV_ARGS(shadowMap.mResource.GetAddressOf())));
		shadowMap.Name = "mShadowMap_" + std::to_string(i);
		auto w = AnsiToWString(shadowMap.Name);
		shadowMap.mResource.Get()->SetName(w.c_str());
		barriers.ResetTrackedState(shadowMap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		mLights[i].mShadowMap = std::make_unique<MCTextureResource>(std::move(shadowMap));
	}
}

void MCLightManager::Shutdown() {
	mLights.clear();
}