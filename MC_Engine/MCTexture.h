#pragma once

#include "MCAssetIdentity.h"
#include "MC_Types.h"  // MCTextureResource (the GPU side)
#include <memory>
#include <string>

// Asset-side texture record. The on-disk .mctex JSON serializes everything above
// the resource line. The MCTextureResource is the GPU realization of this asset
// and its lifetime is tied to the asset's — when MCTextureManager destroys an
// MCTexture, the resource it owns is freed too.
//
// Render targets and post-process textures are NOT MCTextures: they have no
// .mctex file, no asset path, and live as direct MCTextureResource members on
// MCEngine. This struct is specifically "asset-loaded texture from disk."
struct MCTexture {
	// Identity (set at registration; immutable except via MCTextureManager::Rename).
	std::string Name;

	// Source-of-truth fields (round-trip via .mctex JSON).
	std::string assetPath;       // path to the DDS file (relative or absolute)
	bool        sRGB         = true;
	bool        generateMips = true;
	std::string addressU     = "Wrap";
	std::string addressV     = "Wrap";

	// GPU realization. Owned by the asset; null until the manager realizes it.
	// Consumers reach the bindless SRV slot via resource->SRVs[0].offset.
	std::unique_ptr<MCTextureResource> resource;
};
