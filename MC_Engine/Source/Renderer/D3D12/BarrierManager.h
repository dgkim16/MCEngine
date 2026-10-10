#pragma once
#include <d3d12.h>
#include <vector>
#include <assert.h>

#include "Renderer/D3D12/MC_Types.h"
// struct MCResource;

class BarrierManager {
public:
	BarrierManager() = default;
	void TransitionState(MCResource& res, D3D12_RESOURCE_STATES target);
	void InsertUAVBarrier(MCResource& res);

	void SplitTransitionState(MCResource& res, D3D12_RESOURCE_STATES target) { assert(false && "not yet implemented"); }
	void AliasBarrier(MCResource& current, MCResource& target);

	void FlushBarriers(ID3D12GraphicsCommandList* cmdList);

	D3D12_RESOURCE_STATES GetCurrentState(const MCResource& res) const {
		return res.m_currState;
	}
	// Creation-time seed: the state a resource was just created in (or left in by its
	// upload helper). Records no barrier; every later change goes through TransitionState.
	void ResetTrackedState(MCResource& res, D3D12_RESOURCE_STATES state) {
		res.m_currState = state;
	}

private:
	std::vector<D3D12_RESOURCE_BARRIER> mPendingBarriers;
};