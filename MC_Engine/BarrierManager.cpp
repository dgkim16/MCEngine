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