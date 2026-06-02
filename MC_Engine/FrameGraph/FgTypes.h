#pragma once
#include <cstdint>
#include <functional>
#include <dxgiformat.h>
#include <d3d12.h>

// handle used by Fg for accessing Transient & MCResource
struct FgResourceHandle {
	uint32_t id = 0;
	bool IsValid() const { return id != 0; }
	bool operator==(FgResourceHandle rhs) const { return id == rhs.id; }
	bool operator!=(FgResourceHandle rhs) const { return id != rhs.id; }
	bool operator>(const FgResourceHandle rhs) const { return id > rhs.id; }
	bool operator<(const FgResourceHandle rhs) const { return id < rhs.id; }
};

// this allows using FgResourceHandle as key for unordered_map
namespace std {
	// hash implemenation
	template<> struct hash<FgResourceHandle> {
		size_t operator()(FgResourceHandle h) const noexcept { return std::hash<uint32_t>{}(h.id); }
	};
}

struct FgResourceDesc {
	enum class Kind { Texture2D, Buffer } kind = Kind::Texture2D;
	DXGI_FORMAT		format = DXGI_FORMAT_UNKNOWN;
	uint32_t		width		= 0;
	uint32_t		height		= 0;
	uint32_t		arraySize	= 1;
	uint32_t		mipLevels	= 1;
	D3D12_RESOURCE_FLAGS flags	= D3D12_RESOURCE_FLAG_NONE;
	const char*		debugName	= nullptr;
};
