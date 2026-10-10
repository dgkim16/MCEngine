#include "Core/CBFreeList.h"
#include <algorithm>

std::uint32_t CBFreeList::Allocate() {
	if (!mReleased.empty()) {
		std::uint32_t s = mReleased.back();
		mReleased.pop_back();
		return s;
	}
	if (mHighWater >= mCapacity) {
		throw std::runtime_error(
			"CBFreeList exhausted: high-water " + std::to_string(mHighWater) +
			" hit capacity " + std::to_string(mCapacity) + ".");
	}
	return mHighWater++;
}

void CBFreeList::Release(std::uint32_t slot) {
	assert(slot < mHighWater && "Release of never-allocated slot.");
	assert(std::find(mReleased.begin(), mReleased.end(), slot) == mReleased.end()
	       && "Release of an already released slot.");
	mReleased.push_back(slot);
}

void CBFreeList::SetCapacity(std::uint32_t cap) {
	assert(cap >= mHighWater && "SetCapacity below current high-water.");
	mCapacity = cap;
}
