#pragma once
#include <d3d12.h>
#include <vector>
#include <assert.h>

#include "MC_Types.h"
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

private:
	std::vector<D3D12_RESOURCE_BARRIER> mPendingBarriers;
};