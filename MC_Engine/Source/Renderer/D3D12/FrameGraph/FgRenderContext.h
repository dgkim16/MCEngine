#pragma once
#include <d3d12.h>
#include "Renderer/D3D12/FrameGraph/FgTypes.h"

class FrameGraph;
class MCResource;
class BarrierManager;

// Q)	
// Why no FgRenderContext.cpp?
// A)	
// 4 accessors all read graph->mImpl->mTransients, so needs to see Impl struct
// Impl struct is only visible inside FrameGraph.cpp
// therefore FgRenderContext bodies are inside FrameGraph.cpp
// not in its own FgRenderContext.cpp (DNE)

struct FgRenderContext {
	ID3D12GraphicsCommandList* cmdList = nullptr;
	const FrameGraph* graph = nullptr;
	BarrierManager* barriers = nullptr;	// intra-pass transitions and UAV barriers
	uint32_t frameSlot = 0;				// the slot passed to FrameGraph::Begin

	uint32_t GetSRV(FgResourceHandle h) const;
	uint32_t GetUAV(FgResourceHandle h) const;
	ID3D12Resource* GetResource(FgResourceHandle h) const;
	MCResource& GetMCResource(FgResourceHandle h) const;
	template <class T> const T& GetBlackboard() const;
	template <class T> const T* TryGetBlackboard() const;
};