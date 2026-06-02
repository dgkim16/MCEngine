#pragma once
#include <d3d12.h>
#include "FgTypes.h"

class FrameGraph;
class MCResource;

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

	uint32_t GetSRV(FgResourceHandle h) const;
	uint32_t GetUAV(FgResourceHandle h) const;
	ID3D12Resource* GetResource(FgResourceHandle h) const;
	MCResource& GetMCResource(FgResourceHandle h) const;
	template <class T> const T& GetBlackboard() const;
	template <class T> const T* TryGetBlackboard() const;
};