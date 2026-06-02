#pragma once
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include "../MCAssetIdentity.h"
#include "../MC_Types.h"
#include "RenderPass.h"
#include <d3d12.h>

class BarrierManager;
class FgBlackboard;
class CpuTimer;

class FrameGraph	
{
	friend struct FgRenderContext;
public:
	explicit FrameGraph(ID3D12Device* device);
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
		RenderPass* pass;	// borrowed - owned by MCEngine::mPasses
		std::vector<Transition> transitions;
		std::vector<AliasBarrier> aliasBarriers;
	};
	void Begin(uint32_t slot);
	void End();
	void Compile(BarrierManager& bm, CpuTimer& cpuTimer);
	void Execute(ID3D12GraphicsCommandList* cmdList, BarrierManager& bm, GpuTimer& timer);
	void RegisterPass(RenderPass* pass);
	void ComputeLiveSet();
	void DumpToOutputDebugString() const;
	void DumpAliasingPlan(std::function<void(const char*)> sink) const; // sink can be OutputDebugString, or other

	void MarkRoot(MCResource& res);
	FgBlackboard& Blackboard(); // used by RenderPassBuilder to access mImpl->FgBlackboard
	const FgBlackboard& Blackboard() const;
	// called by FgBuilder
	void RecordUsage(uint32_t passIdx, MCResource* res,      D3D12_RESOURCE_STATES req, bool isWrite);
	void RecordUsage(uint32_t passIdx, FgResourceHandle h,   D3D12_RESOURCE_STATES req, bool isWrite);
	FgResourceHandle CreateTransient(const FgResourceDesc& desc);
private:
	std::vector<RenderPass*> mBorrowedPasses;	// borrowed from MCEngine class
	std::vector<CompiledPass> mCompiled; // per-frame, RenderPass (ref) wrapper
	struct Impl;
	std::unique_ptr<Impl> mImpl;
	static auto ClassifyTransient(const FgResourceDesc& d);
	void SizeAndAllocateTransientHeaps();
	void CreatePlacedResources(); // Transients are placed resources
	void ComputeTransientLifetimes(const std::vector<uint32_t>& execOrder);
	void SolveAliasing(); // Greedy Interval coloring per heap class (rtvdsv/uav/srv)
// ========== imgui visualization
public:
	struct FgPanelRow {
		const char* name;        // pass->TypeName()
		uint32_t    reads;
		uint32_t    writes;
		const char* aliasOver;   // nullptr if not aliased
		bool        culled;
		float       gpuMs;
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
