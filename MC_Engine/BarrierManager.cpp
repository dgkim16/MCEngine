#include "BarrierManager.h"

bool CanPromoteNonSim(D3D12_RESOURCE_STATES target) {
	if(target == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE ||
		target == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
		target == D3D12_RESOURCE_STATE_COPY_DEST || 
		target == D3D12_RESOURCE_STATE_COPY_SOURCE)
		return true;
	return false;
}

void BarrierManager::TransitionState(MCResource& res, D3D12_RESOURCE_STATES target) {
	if (res.m_currState == target) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = res.mResource.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = res.m_currState;
	barrier.Transition.StateAfter = target;
	mPendingBarriers.push_back(barrier);
	res.m_currState = target;
}

void BarrierManager::InsertUAVBarrier(MCResource& res) {
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.UAV.pResource = res.mResource.Get();
	mPendingBarriers.push_back(barrier);
}

void BarrierManager::FlushBarriers(ID3D12GraphicsCommandList* cmdList) {
	if(mPendingBarriers.empty())
		return;
	cmdList->ResourceBarrier(static_cast<UINT>(mPendingBarriers.size()), mPendingBarriers.data());
	mPendingBarriers.clear();
}

void BarrierManager::AliasBarrier(MCResource& current, MCResource& target) {
	D3D12_RESOURCE_BARRIER bar = {};
	bar.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
	bar.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	bar.Aliasing.pResourceBefore = current.mResource.Get();
	bar.Aliasing.pResourceAfter = target.mResource.Get();
	mPendingBarriers.push_back(bar);
	// After an aliasing barrier the activated placed resource is freshly (re)activated
	// at COMMON — its contents and state are undefined. Compile seeds the transient's
	// entry transition from COMMON (GetCurrentState on the just-created placed resource),
	// so the post-alias state must be COMMON for that planned transition to carry the
	// correct StateBefore. Inheriting the predecessor's state corrupts the before-state
	// and leaves the resource in COMMON when it is next bound (GBV incompatible-layout).
	// target.m_currState = D3D12_RESOURCE_STATE_COMMON;
	// target.m_currState = current.m_currState;
}
