#pragma once

#include "MCAssetIdentity.h"
#include "Common/d3dUtil.h"     // gNumFrameResources
#include "Common/MathHelper.h"  // Identity4x4()
#include <DirectXMath.h>
#include <string>

struct MCMaterial {
	// Identity (set at registration; immutable except via MCMaterialManager::Rename).
	std::string Name;
	int MatCBIndex = -1;          // assigned by MCMaterialManager via CBFreeList

	// Texture reference — handle, not name. SRV slot is resolved per-frame
	// via mTextureManager.Get(textureHandle)->resource->SRVs[0].offset at
	// the shader-input write site (MCEngine.cpp's UpdateMaterialCBs).
	TextureHandle textureHandle = 0;

	// Author-intent fields (round-trip via .mcmat JSON).
	DirectX::XMFLOAT4   DiffuseAlbedo  = { 1.0f, 1.0f, 1.0f, 1.0f };	// SharedTypes.h, MaterialConstants
	DirectX::XMFLOAT3   FresnelR0      = { 0.05f, 0.05f, 0.05f };		// SharedTypes.h, MaterialConstants
	int                 FresnelIndex   = 6;
	float               Roughness      = 0.25f;							// SharedTypes.h, MaterialConstants
	DirectX::XMFLOAT4X4 MatTransform   = MathHelper::Identity4x4();		// SharedTypes.h, MaterialConstants

	// Frame state (derived).
	int NumFramesDirty = gNumFrameResources;
};
