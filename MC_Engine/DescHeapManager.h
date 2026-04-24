#pragma once
#include "d3dUtil.h"
#include "Singleton.h"
#include "MC_Types.h"
#include <unordered_set>


// assumes mix of bindless (dxc, hlsl 6.6), meaning...
// use of StructuredBuffer for buffer_srv and uav
// use of ResourceDescriptorHeap[idx] for texture_srvs and texture_uavs
// Constant buffer Resources directly binding via SetRootConstantBufferView (without using CBVs in descriptorHeap)

#define CPUH CD3DX12_CPU_DESCRIPTOR_HANDLE
#define GPUH CD3DX12_GPU_DESCRIPTOR_HANDLE
#define DH ID3D12DescriptorHeap

struct DHSlotInfo {
	std::string  resourceName; // from MCResource::Name
	const char* viewKind = "";// "SRV2D","SRVCube","SRVBuf","UAV2D","UAVBuf","RTV","DSV","CBV","-"
	DXGI_FORMAT  format = DXGI_FORMAT_UNKNOWN; // 0 for buffers
};

struct DHInfo {
	std::unordered_set<int> Offset_Set;
	std::vector<int> freeList;
	UINT descSize = 0;
	int MaxCapacity = 0;
	int lastOffset = 0;
	int reservedHead = 0;          // leading slots owned externally (e.g. D3DApp swap-chain RTVs)
	const char* debugName = "?";   // set by DescHeapManager ctor: "Static"/"Dynamic"/"Rtv"/"Dsv"
	std::vector<DHSlotInfo> slotInfo;

	struct FreePendingList {
		int idx;
		int counter;
	};
	std::vector<FreePendingList> fpl;

	DHInfo() = default;
	DHInfo(UINT dS, int mC) : descSize(dS), MaxCapacity(mC) { slotInfo.resize(mC); }

	// returns offset (index) where allocation can be done.
	// actual allocation must be done by the caller of this function with the return value.
	// if there is an entry in free list, consume that value as the target idx for alloc
	// if there is no entry in free list, set target idx for alloc as the last offset (<maxcap)
	int allocate() {
		if (!freeList.empty()) {
			int idx = freeList.back();
			freeList.pop_back();
			return idx;
		}
		if (lastOffset >= MaxCapacity) {
			char buf[256];
			std::snprintf(buf, sizeof(buf),
				"[DHInfo] CAPACITY EXCEEDED tier=%s lastOffset=%d MaxCapacity=%d fpl.size=%zu freeList.size=%zu\n",
				debugName, lastOffset, MaxCapacity, fpl.size(), freeList.size());
			OutputDebugStringA(buf);
		}
		assert(lastOffset < MaxCapacity && "Descriptor Heap Capacity Exceeded!");
		return lastOffset++;
	}
};

// Per-CBV/SRV/UAV-type aggregate. Writes land in the tier's CPU-only staging heap;
// CommitToShaderVisible copies dirty tiers into the single shader-visible combined heap.
// Bindless indices are stable: global = baseOffset[tier] + localOffsetInTier.
struct TierCSU {
	ComPtr<ID3D12DescriptorHeap> cpuHeap[MC_VIEW_TIER_COUNT];    // owned by TierCSU.	CPU-only staging heaps for each tier
	DH* combined = nullptr;                                      // non-owning.			shader-visible combined heap
	DHInfo tiers[MC_VIEW_TIER_COUNT];                            // per-tier allocator	(local offsets)
	int baseOffset[MC_VIEW_TIER_COUNT] = {};            // per-tier baseOffset (global idx of when local tier idx = 0) fixed at init
	bool dirty[MC_VIEW_TIER_COUNT] = {};                // set by Create*, cleared by CommitToShaderVisible
	UINT descSize = 0;
	int reservedHead = 0;
};

class DescHeapManager final : public Singleton<DescHeapManager>
{
	friend class Singleton<DescHeapManager>;
private:
	DescHeapManager() = delete;
	DescHeapManager(MC_DESC_HEAP_MANAGER_DESC desc);

public:
	void Update();

	// Copy each dirty tier from its CPU-only staging heap into the combined shader-visible heap.
	// Call once per frame before SetDescriptorHeaps on the draw command list.
	void CommitToShaderVisible();

	// Drain all pending-free entries into freeList immediately. Caller MUST have already
	// made the GPU idle (e.g. via FlushCommandQueue). Use on resize paths so new allocations
	// reuse the same slots instead of walking lastOffset forward — keeps any GPU handles
	// cached earlier this frame (e.g. ImGui's ImTextureID) valid across the re-create.
	void FlushPending();

	// Release owned descriptor heaps so they don't outlive MCEngine's ReportLiveObjects.
	// Singleton instance itself stays alive until program exit; this just empties its ComPtrs.
	void Shutdown();

	void CreateCbv( MCBuffer& buffer, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);

	void CreateSrv2d( MCTexture& resource, DXGI_FORMAT format, bool isMSAA = false, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);
	void CreateSrvCube( MCTexture& resource, DXGI_FORMAT format, bool isMSAA = false, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);
	void CreateSrvBuffer( MCBuffer& buffer, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);

	void CreateUav2d( MCTexture& resource, DXGI_FORMAT format, UINT mipSlice = 0, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);
	void CreateUavBuffer( MCBuffer& resource, ID3D12Resource* counterResource = nullptr, UINT64 counterOffset = 0, MC_VIEW_TIER tier = MC_VIEW_TIER::MC_VIEW_TIER_STATIC);

	// RTV/DSV are CPU-only (not shader-visible) and never tiered; no copy step.
	void CreateRtv2d( MCTexture& resource, DXGI_FORMAT format, UINT mipSlice = 0);
	void CreateDsv( MCTexture& resource, D3D12_DSV_FLAGS flags, DXGI_FORMAT format, bool isMSAA = false, UINT mipSlice = 0);

	void QueueRemovalFromSet(int idx, DHInfo& map);
	void QueueRemovalFromSet_CbvSrvUav(DescHeapHandle h);   // translates global->local via h.tier
	void QueueRemovalFromSet_Rtv(DescHeapHandle h) { return QueueRemovalFromSet(h.offset, mRtv); }
	void QueueRemovalFromSet_Dsv(DescHeapHandle h) { return QueueRemovalFromSet(h.offset, mDsv); }

	void QueueRemoval_Texture(MCTexture& resource);
	void QueueRemoval_Buffer(MCBuffer& resource);

	// read-only accessors (used by ImGui UI)
	const TierCSU& GetCSU() const { return mCSU; }
	const DHInfo& GetRtv() const { return mRtv; }
	const DHInfo& GetDsv() const { return mDsv; }
	int GetCsuReservedHead() const { return mCSU.reservedHead; }

private:
	void UpdatePending(DHInfo& dhi);
	void RemoveFromSet(int idx, DHInfo& map);

	// CPU handle into a tier's CPU-only staging heap at a tier-local offset (copy source).
	CPUH hCSU_Stage(int tierIdx, int localOff) {
		return CPUH(mCSU.cpuHeap[tierIdx]->GetCPUDescriptorHandleForHeapStart(), localOff, mCSU.descSize);
	}
	// GPU handle into the combined shader-visible heap at a global offset (shader sees this).
	GPUH hCSU_CombinedGpu(int globalOff) {
		return GPUH(mCSU.combined->GetGPUDescriptorHandleForHeapStart(), globalOff, mCSU.descSize);
	}

	CPUH hDescRtv(int off) { return CPUH(mRtvHeap->GetCPUDescriptorHandleForHeapStart(), off, mRtv.descSize); }
	CPUH hDescDsv(int off) { return CPUH(mDsvHeap->GetCPUDescriptorHandleForHeapStart(), off, mDsv.descSize); }

private:
	ID3D12Device* device; // non-owning

	DH* mRtvHeap; // non-owning
	DH* mDsvHeap; // non-owning
	int NumFrameResources;

	TierCSU mCSU;
	DHInfo mRtv;
	DHInfo mDsv;
};
