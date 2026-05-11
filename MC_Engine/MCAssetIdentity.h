/*
A simple Hashing program.
Takes AssetKind and the string (name) and creates a hash.
*/

#pragma once
#define XXH_INLINE_ALL
#include "Includes/xxhash/xxhash.h"
#include <cstdint>
#include <string_view>


enum class AssetKind : std::uint32_t {
	Material = 1,
	MeshSource = 2,
	Texture = 3,
};

template <AssetKind K>
inline std::uint64_t HashAssetIdentity(std::string_view displayName) {
	XXH64_state_t* st = XXH64_createState();
	XXH64_reset(st, 0);
	std::uint32_t tag = static_cast<std::uint32_t>(K);
	XXH64_update(st, &tag, sizeof(tag));						// feed tag into hash state
	XXH64_update(st, displayName.data(), displayName.size());	// feed name into hash state
	auto h = XXH64_digest(st);						// let xxhash 'digest' what was fed
	XXH64_freeState(st);
	return h;
}

// these are for hashed values (created via MCAssetIdentity) stored as uint64_t
using MaterialHandle = std::uint64_t;
using MeshHandle = std::uint64_t;
using TextureHandle = std::uint64_t;