#include "CBFreeList.h"

std::uint32_t CBFreeList::Allocate() {
	if (!mReleased.empty()) {
		std::uint32_t s = mReleased.top();
		mReleased.pop();
		return s;
	}
	if (mHighWater >= mCapacity) {
		throw std::runtime_error(
			"CBFreeList exhausted: high-water " + std::to_string(mHighWater) +
			" hit capacity " + std::to_string(mCapacity) +
			". Increase headroom in BuildFrameResources or implement CB growth.");
	}
	return mHighWater++;
}

void CBFreeList::Release(std::uint32_t slot) {
	assert(slot < mHighWater && "Release of never-allocated slot.");
	mReleased.push(slot);
}

void CBFreeList::SyncHighWaterFromBuild(std::uint32_t initialItemCount) {
	assert(mHighWater == 0 && "SyncHighWaterFromBuild called twice.");
	mHighWater = initialItemCount;
}

void CBFreeList::SetCapacity(std::uint32_t cap) {
	assert(cap >= mHighWater && "SetCapacity below current high-water.");
	mCapacity = cap;
}

void CBFreeList::Reset() {
	assert(false && "Not yet implemented.");
}
