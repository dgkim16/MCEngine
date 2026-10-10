#pragma once
#include <cstdint>
#include <vector>
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
	// Debug asserts that the slot is not already in the pool.
	void Release(std::uint32_t slot);

	// Called once, before the first Allocate. The capacity is fixed from then
	// on: Allocate throws once the high-water mark reaches it.
	void SetCapacity(std::uint32_t cap);

	std::uint32_t HighWater() const { return mHighWater; }
	std::uint32_t Capacity() const { return mCapacity; }
private:
	std::vector<std::uint32_t> mReleased;	// used as a stack: LIFO reuse
	std::uint32_t			 mHighWater = 0;
	std::uint32_t			 mCapacity = 0;
};
