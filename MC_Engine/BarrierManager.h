#pragma once
#include <d3d12.h>
#include <vector>
#include <assert.h>

#include "MC_Types.h"
// struct MCResource;

class BarrierManager {
public:
	BarrierManager(ID3D12GraphicsCommandList* m_cmdList) : cmdList(m_cmdList) {};
	void TransitionState(MCResource& res, D3D12_RESOURCE_STATES target, bool immediate = false);
	void InsertUAVBarrier(MCResource& res);

	void SplitTransitionState(MCResource& res, D3D12_RESOURCE_STATES target) { assert(false, "not yet implemented"); }
	void AliasBarrier(MCResource& current, MCResource& target)				 { assert(false, "not yet implemented"); }

	void FlushBarriers(ID3D12GraphicsCommandList* cmdList);

private:
	ID3D12GraphicsCommandList* cmdList;
	std::vector<D3D12_RESOURCE_BARRIER> mPendingBarriers;
};