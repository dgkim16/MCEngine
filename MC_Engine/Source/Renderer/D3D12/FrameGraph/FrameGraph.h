#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include "Renderer/D3D12/MC_Types.h"
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include <d3d12.h>

class BarrierManager;
class FgBlackboard;
class CpuTimer;
class GpuTimer;
class DescHeapManager;

class FrameGraph	
{
	friend struct FgRenderContext;
public:
	explicit FrameGraph(ID3D12Device* device, DescHeapManager& descHeapManager, GpuTimer& timer);
	~FrameGraph();
	FrameGraph(const FrameGraph&) = delete; // copy ctor delete
	FrameGraph& operator=(const FrameGraph&) = delete; // copy assignment op delete
	FrameGraph(FrameGraph&&) = delete; // move ctor delete
	FrameGraph& operator=(FrameGraph&&) = delete; // move assigment delete
public:
	struct CompiledPass { // RenderPass (ref)	wrapper
		struct Transition {
			MCResource* resource;
			D3D12_RESOURCE_STATES targetState;
		};
		struct AliasBarrier {
			MCResource* before;
			MCResource* after;
		};
		RenderPass* pass;	// borrowed - owned by the caller of RegisterPass
		uint32_t regIdx;	// registration index: mBorrowedPasses / timer ids
		std::vector<Transition> transitions;
		std::vector<AliasBarrier> aliasBarriers;
	};
	void Begin(uint32_t slot);
	void End();
	void Compile(BarrierManager& bm, CpuTimer& cpuTimer);
	void Execute(ID3D12GraphicsCommandList* cmdList, BarrierManager& bm, GpuTimer& timer);
	void RegisterPass(RenderPass* pass);
#if defined(_DEBUG)
	// Execute calls the hook before every pass, after the pass's barriers are flushed and
	// before its timer scope opens. An empty function removes it. Debug builds only.
	void SetDebugPrePassHook(std::function<void(ID3D12GraphicsCommandList*, const RenderPass&)> hook);
#endif
	void ComputeLiveSet();
	void DumpToOutputDebugString() const;
	void DumpToOutputDebugString(std::function<void(const char*)> sink) const; // same text, any sink (the capture sweep writes it to a file)
	void DumpAliasingPlan(std::function<void(const char*)> sink) const; // sink can be OutputDebugString, or other

	void MarkRoot(MCResource& res);
	FgBlackboard& Blackboard(); // used by RenderPassBuilder to access mImpl->FgBlackboard
	const FgBlackboard& Blackboard() const;
	// called by FgBuilder
	void RecordUsage(uint32_t passIdx, MCResource* res,      D3D12_RESOURCE_STATES req, bool isWrite);
	void RecordUsage(uint32_t passIdx, FgResourceHandle h,   D3D12_RESOURCE_STATES req, bool isWrite);
	FgResourceHandle CreateTransient(const FgResourceDesc& desc);
private:
	std::vector<RenderPass*> mBorrowedPasses;	// borrowed from the caller of RegisterPass
	std::vector<CompiledPass> mCompiled; // per-frame, RenderPass (ref) wrapper
	struct Impl;
	std::unique_ptr<Impl> mImpl;
	static auto ClassifyTransient(const FgResourceDesc& d);
	void SizeAndAllocateTransientHeaps();
	void CreatePlacedResources(BarrierManager& bm); // Transients are placed resources
	void ComputeTransientLifetimes(const std::vector<uint32_t>& execOrder);
	void SolveAliasing(); // Greedy Interval coloring per heap class (rtvdsv/uav/srv)
// ========== imgui visualization
public:
	struct FgPanelRow {
		struct Usage {
			const char* resource; // MCResource::Name copied by the last Compile; valid until the next Compile
			bool        isWrite;
		};
		const char* name;        // pass->TypeName()
		uint32_t    reads;
		uint32_t    writes;
		const char* aliasOver;   // nullptr if not aliased
		bool        culled;
		float       gpuMs;
		uint32_t    execIdx;     // UINT32_MAX when culled
		std::vector<Usage> usages; // Setup declaration order
	};
	struct FgPanelData {
		std::vector<FgPanelRow> rows;
		uint64_t bytesAliased;
		uint64_t bytesNoAlias;
		uint32_t liveCount;
		uint32_t culledCount;
		float    compileMs;      // -1.f until Step 3.1 wires it
	};
	FgPanelData BuildPanelSnapshot(const GpuTimer& timer) const;
};


template <class T> T& RenderPassBuilder::AddBlackboard() { return mGraph->Blackboard().Add<T>(); }
template <class T> T& RenderPassBuilder::GetBlackboard() { return mGraph->Blackboard().Get<T>(); }
template <class T> T* RenderPassBuilder::TryGetBlackboard() { return mGraph->Blackboard().TryGet<T>(); }
template <class T> const T& FgRenderContext::GetBlackboard() const { return graph->Blackboard().Get<T>(); }
template <class T> const T* FgRenderContext::TryGetBlackboard() const { return graph->Blackboard().TryGet<T>(); }
