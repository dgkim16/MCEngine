#pragma once
#include <cstdint>
#include <stack>
#include <stdexcept>
#include <string>
#include <cassert>

// Generic per-frame constant-buffer slot allocator. Used both for the object CB
// pool (one slot per RenderItem) and the material CB pool (one slot per
// Material). Headroom is set per-instance at SetCapacity() time — the class is
// pool-agnostic.
class CBFreeList {
public:
	// Hands out the next free slot. Prefers released slots (LIFO) over growing
	// the high-water mark. Throws if both are exhausted.
	std::uint32_t Allocate();

	// Returns the slot to the released pool. Caller must guarantee no in-flight
	// frame still references it — see "No release ring" in Phase1_Week3_Day5.md
	void Release(std::uint32_t slot);

	// Should be called once after MCScene::Load + the renumber pass at MCEngine.cpp:164-165.
	// Establishes the high-water mark to match what BuildRenderItems already populated.
	void SyncHighWaterFromBuild(std::uint32_t initialItemCount);

	// Called once after BuildFrameResources sizes the CB. Capacity must be
	// strictly greater than the boot-time item count so LoadRenderItemFromJson
	// has slots to grow into. After this call, CB is fixed size.
	void SetCapacity(std::uint32_t cap);

	void Reset();

	std::uint32_t HighWater() const { return mHighWater; }
	std::uint32_t Capacity() const { return mCapacity; }
private:
	std::stack< std::uint32_t> mReleased;
	std::uint32_t			 mHighWater = 0;
	std::uint32_t			 mCapacity = 0;
};
