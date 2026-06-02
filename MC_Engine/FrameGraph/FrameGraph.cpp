#include <d3d12.h>
#include <unordered_set>
#include <queue>
#include <stdexcept>
#include <debugapi.h> // for OutputDebugStringA
#include <string>
#include <algorithm>   // for std::find
#include "FrameGraph.h"
#include "RenderPass.h"
#include "../GpuTimer.h"
#include "../BarrierManager.h"
#include "../MC_Types.h"
#include <variant>
#include "FgTypes.h"
#include "../ConstExpressionValues.h"
#include "../DescHeapManager.h"
#include "FgBlackboard.h"
#include "../CpuTimer.h"

static bool dumping = false; // is turned off at end of compile. turned on when calling public FraneGraph::Dump...() functions

// Local replacement for ThrowIfFailed — that macro isn't available in this TU.
// Behaves the same : log on failure, break in debug, throw to propagate up.
static inline void FgCheck(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[FrameGraph] %s failed: 0x%08lX\n",
            what, static_cast<unsigned long>(hr));
        OutputDebugStringA(buf);
        DebugBreak();
        throw std::runtime_error(buf);
    }
}

struct FrameGraph::Impl {
    struct ResourceUsage { // MCResource (by ptr ref) wrapper
        uint32_t pass;
        std::variant<MCResource*, FgResourceHandle> resource;
        D3D12_RESOURCE_STATES required;
        bool isWrite;
    };

    struct TransientEntry {
        FgResourceDesc desc;
        std::unique_ptr<MCResource> resource;
        D3D12_RESOURCE_DESC d3dDesc = {}; // cached inside entry to avoid rebuild
        uint64_t alignment = 0;
        uint64_t sizeInBytes = 0;
        uint32_t firstUsePassIdx = UINT32_MAX;
        uint32_t lastUsePassIdx = 0;
        uint32_t heapOffset = 0;
        enum class HeapClass { RtvDsv, UavBuffer, TextureOnly } heapClass = HeapClass::TextureOnly;
    };

    FgBlackboard mBlackboard;

    std::vector<std::vector<ResourceUsage>> mPassUsages; // per-pass reads + writes from Setup. N passes -> [N][#resources used by that pass]
    std::array<std::unordered_map<FgResourceHandle, TransientEntry>,gNumFrameResources> mTransientsPerFrame;
    std::vector<MCResource*> mRoots;
    std::vector<bool> mLiveSet;
    std::array<uint32_t, gNumFrameResources> mNextHandleIdPerSlot{ 1, 1, 1 };
    uint32_t mCurrSlot = 0;

    ID3D12Device* mDevice = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Heap> mHeapRtvDsv; // ALLOW_RENDER_TARGET | ALLOW_DEPTH_STENCIL
    Microsoft::WRL::ComPtr<ID3D12Heap> mHeapUavBuffer; // ALLOW_UNORDERED_ACCESS, BUFFER
    Microsoft::WRL::ComPtr<ID3D12Heap> mHeapTextureOnly; // no special flags, TEXTURE
    uint64_t mHeapHighWater[3] = { 0,0,0 }; // sizes the heaps were created at; growth requires re-create
    uint64_t mPerSlotStride[3] = { 0,0,0 }; // 64KB-aligned bytes per frame slot; heap = stride * gNumFrameResources.

    struct Bin { // bin of transients 
        uint32_t                       lastUsePassIdx = 0;   // exec-order
        uint64_t                       sizeInBytes = 0;   // max among occupants' sizeInBytes
        uint64_t                       offset = 0;
        std::vector<FgResourceHandle>  occupants;
    };

    std::array<std::vector<Bin>, 3>  mBinsPerClass;       // one bin list per heap class; populated by SolveAliasing
    std::array<uint64_t, 3>          mSolverHeapTotals{}; // total heap bytes per class (post-alignment); populated by SolveAliasing
};

auto FrameGraph::ClassifyTransient(const FgResourceDesc& d) {
    using HC = Impl::TransientEntry::HeapClass;
    if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) return HC::RtvDsv;
    if (d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) return HC::RtvDsv;
    if (d.kind == FgResourceDesc::Kind::Buffer) return HC::UavBuffer;
    return HC::TextureOnly;
}

FgResourceHandle FrameGraph::CreateTransient(const FgResourceDesc& desc) {
    FgResourceHandle h{ mImpl->mNextHandleIdPerSlot[mImpl->mCurrSlot]++ };
    Impl::TransientEntry entry{};
    entry.desc = desc;
    entry.heapClass = ClassifyTransient(desc);
    entry.resource = (desc.kind == FgResourceDesc::Kind::Buffer)
        ? std::unique_ptr<MCResource>(std::make_unique<MCBufferResource>())
        : std::unique_ptr<MCResource>(std::make_unique<MCTextureResource>());
    if (desc.debugName) entry.resource->Name = desc.debugName;
    mImpl->mTransientsPerFrame[mImpl->mCurrSlot].emplace(h, std::move(entry));
    return h;
}

// a setup stage before actual placed resource allocation to specific heap region
void FrameGraph::SizeAndAllocateTransientHeaps() {
    auto& s = *mImpl;
    // 1. Walk every transient to determine size
    uint64_t sumPerClass[3] = { 0,0,0 }; // per-class running sum
    for (auto& [handle, entry] : s.mTransientsPerFrame[s.mCurrSlot]) {
        // 1A. build transient's D3D12_RESOURCE_DESC
        if (entry.desc.kind == FgResourceDesc::Kind::Buffer) {
            entry.d3dDesc = CD3DX12_RESOURCE_DESC::Buffer(entry.desc.width, entry.desc.flags);
        } else {
            entry.d3dDesc = CD3DX12_RESOURCE_DESC::Tex2D(
                entry.desc.format,
                entry.desc.width,
                entry.desc.height,
                static_cast<UINT16>(entry.desc.arraySize),
                static_cast<UINT16>(entry.desc.mipLevels),
                1, /*sampleCount*/
                0, /*sampleQuality*/
                entry.desc.flags);
        }

        // 1B. query the device for SizeInBytes + Alignment & store on TransientEntry
        const D3D12_RESOURCE_ALLOCATION_INFO info = 
            s.mDevice->GetResourceAllocationInfo(/*node*/ 0, /*count*/ 1, &entry.d3dDesc);
        entry.sizeInBytes = info.SizeInBytes;

        // 1C. Add into per-class (per-heaptype) running sum
        const int classIdx = static_cast<int>(entry.heapClass);
        const uint64_t alignment = info.Alignment;
        // round the running cursor up to a multiple of alignment
        sumPerClass[classIdx] = (sumPerClass[classIdx] + alignment - 1) & ~(alignment - 1);
        // bump the cursor past this resource. The next iteration will round up again from there
        sumPerClass[classIdx] += entry.sizeInBytes;
    }
    // 2. Create heaps lazily at the first Compile that needs each one.
    static constexpr D3D12_HEAP_FLAGS kHeapFlags[3] = {
        D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES,       // RtvDsv
        D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,              // UavBuffer
        D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES,   // TextureOnly
    };
    static constexpr const wchar_t* kHeapNames[3] = {
        L"FrameGraph::Heap[RtvDsv]",
        L"FrameGraph::Heap[UavBuffer]",
        L"FrameGraph::Heap[TextureOnly]",
    };
    Microsoft::WRL::ComPtr<ID3D12Heap>* heaps[3] = {
        &s.mHeapRtvDsv, &s.mHeapUavBuffer, &s.mHeapTextureOnly
    };

    for (int c = 0; c < 3; ++c) {
        if (sumPerClass[c] == 0) continue;  // class has no transients this Compile
        if (*heaps[c]) {
            if (sumPerClass[c] > s.mHeapHighWater[c]) { // growth is yet unsupported
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "[FG] heap class %d high-water exceeded: needed %llu, cap %llu.\n",
                    c,
                    static_cast<unsigned long long>(sumPerClass[c]),
                    static_cast<unsigned long long>(s.mHeapHighWater[c]));
                OutputDebugStringA(msg);
                DebugBreak();
            }
            continue;
        }

        constexpr uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64KB
        s.mPerSlotStride[c] = (sumPerClass[c] + kAlign - 1) & ~(kAlign - 1);

        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = sumPerClass[c] * gNumFrameResources;
        heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heapDesc.Alignment = 0;
        heapDesc.Flags = kHeapFlags[c];
        FgCheck(s.mDevice->CreateHeap(
            &heapDesc, IID_PPV_ARGS(heaps[c]->ReleaseAndGetAddressOf())),
            "CreateHeap");
        (*heaps[c])->SetName(kHeapNames[c]);
        s.mHeapHighWater[c] = sumPerClass[c];
    }
}

// called immediately after SizeAndAllocateTransientHeaps()
// creates placed resources only for transient resources (created/dies every frame)
void FrameGraph::CreatePlacedResources() {
    auto& s = *mImpl;
    ID3D12Heap* heaps[3] = {
        s.mHeapRtvDsv.Get(), s.mHeapUavBuffer.Get(), s.mHeapTextureOnly.Get()
    };
    auto& dhm = DescHeapManager::Get();
    for (auto& [handle, entry] : s.mTransientsPerFrame[s.mCurrSlot]) {
        if (entry.firstUsePassIdx == UINT32_MAX) continue;

        const int classIdx = static_cast<int>(entry.heapClass);
        ID3D12Heap* heap = heaps[classIdx];
        if (!heap) {
            OutputDebugStringA("[FG] transient's heap class has no heap; Step 1d bookkeeping is broken\n");
            DebugBreak();
            continue;
        }
        const uint64_t slotBase = s.mPerSlotStride[classIdx] * s.mCurrSlot;
        FgCheck(
            s.mDevice->CreatePlacedResource(
                heap,
                slotBase + entry.heapOffset, // offsetPerClass[classIdx],
                &entry.d3dDesc,
                D3D12_RESOURCE_STATE_COMMON,   // only legal initial state for placed resource
                /*pOptimizedClearValue*/ nullptr,
                IID_PPV_ARGS(entry.resource->mResource.ReleaseAndGetAddressOf())
            ),
            "CreatePlacedResource"
        );
        // Name for PIX. debugName has string-literal lifetime per deviation #8.
        if (entry.desc.debugName) {
            wchar_t wname[128];
            MultiByteToWideChar(CP_UTF8, 0, entry.desc.debugName, -1,
                wname, _countof(wname));
            entry.resource->mResource->SetName(wname);
            entry.resource->Name = entry.desc.debugName;
        }
        entry.resource->m_currState = D3D12_RESOURCE_STATE_COMMON;

        if (entry.desc.kind == FgResourceDesc::Kind::Buffer) {
            auto& buf = static_cast<MCBufferResource&>(*entry.resource);
            dhm.CreateSrvBuffer(buf, MC_VIEW_TIER_DYNAMIC);
            if(entry.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                dhm.CreateUavBuffer(buf, /*counter*/nullptr, 0, MC_VIEW_TIER_DYNAMIC);
        } else {
            auto& tex = static_cast<MCTextureResource&>(*entry.resource);
            dhm.CreateSrv2d(tex, entry.desc.format, /*isMSAA*/false, MC_VIEW_TIER_DYNAMIC);
            if (entry.desc.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                dhm.CreateUav2d(tex, entry.desc.format, /*mip*/0, MC_VIEW_TIER_DYNAMIC);
        }
       
    }
}

// updates mImpl->mTransients[FgResourceHandle].firstUsePassIdx/lastUsePassIdex
void FrameGraph::ComputeTransientLifetimes(const std::vector<uint32_t>& execOrder) {
    /*
    Walk mImpl->mPassUsages (nested per-pass) and populate each transient's 
    firstUsePassIdx / lastUsePassIdx as execution-order indices, by converting 
    u.pass (registration-order) through execOrder at population time. 
    Storing exec-order directly means the solver's interval comparisons never re-convert:
    */
    for (auto& [handle, entry] :mImpl->mTransientsPerFrame[mImpl->mCurrSlot]) {
        entry.firstUsePassIdx = UINT32_MAX;
        entry.lastUsePassIdx = 0;
    }
    const uint32_t N = static_cast<uint32_t>(mBorrowedPasses.size());
    for (uint32_t p = 0; p < N; ++p) {
        if (!mImpl->mLiveSet[p]) continue;
        const uint32_t execIdx = execOrder[p];
        for (const auto& u : mImpl->mPassUsages[p]) {
            if (!std::holds_alternative<FgResourceHandle>(u.resource)) continue;
            FgResourceHandle h = std::get<FgResourceHandle>(u.resource);
            auto& entry = mImpl->mTransientsPerFrame[mImpl->mCurrSlot].at(h);
            entry.firstUsePassIdx = (std::min)(entry.firstUsePassIdx, execIdx);
            entry.lastUsePassIdx = (std::max)(entry.lastUsePassIdx, execIdx);
        }
    }
}

// updates mTransients.at(handle).heapOffset and Bin
// each Bin = alias (no lifetime overlap)
// function is optimized to minimizing nubmer of Bins, which maximizes aliased memory
void FrameGraph::SolveAliasing() {
    using HC = Impl::TransientEntry::HeapClass;
    std::array<HC, 3> hcArr{ HC::RtvDsv, HC::UavBuffer, HC::TextureOnly };
    for (auto hc : hcArr) {
        const int classIdx = static_cast<int>(hc);
        auto& bins = mImpl->mBinsPerClass[classIdx];

        std::vector<FgResourceHandle> hv;
        for (auto& [h, entry] : mImpl->mTransientsPerFrame[mImpl->mCurrSlot]) {
            if (hc == ClassifyTransient(entry.desc) && entry.firstUsePassIdx != UINT32_MAX)
                hv.emplace_back(h);
        }
        // sort by entry.firstUsePassIdx
        std::sort(hv.begin(), hv.end(), [&](FgResourceHandle a, FgResourceHandle b) {
            const auto& ea = mImpl->mTransientsPerFrame[mImpl->mCurrSlot].at(a);
            const auto& eb = mImpl->mTransientsPerFrame[mImpl->mCurrSlot].at(b);
            return (ea.firstUsePassIdx != eb.firstUsePassIdx) ? ea.firstUsePassIdx < eb.firstUsePassIdx : a.id < b.id;
            }
        );
        /*
        For each transient handle in sorted order, look up the entry in mImpl->mTransients once into a local auto& entry. Then:
        Walk active bins; find the lowest-numbered bin whose lastUsePassIdx < entry.firstUsePassIdx. 
        Assign the transient to that bin: append handle to occupants, set bin's lastUsePassIdx = max(bin.lastUsePassIdx, 
        entry.lastUsePassIdx), set bin's size = max(bin.size, entry.sizeInBytes).
        If no such bin exists, allocate a new bin with this transient as the sole occupant 
        (size = entry.alignedSize, lastUsePassIdx = entry.lastUsePassIdx).
        */
        auto CreateNewBin = [&](FgResourceHandle& h, FrameGraph::Impl::TransientEntry& entry){ 
            Impl::Bin newBin;
            newBin.lastUsePassIdx = entry.lastUsePassIdx;
            newBin.sizeInBytes = entry.sizeInBytes;
            newBin.offset = 0;
            newBin.occupants = std::vector<FgResourceHandle>{ h };
            bins.emplace_back(newBin);
           };
        for (auto& h : hv) {
            auto& entry = mImpl->mTransientsPerFrame[mImpl->mCurrSlot].at(h);
            int lowestBinIdx = -1;
            for (auto i = 0; i < bins.size(); ++i) {
                auto& bin = bins[i];
                if (bin.lastUsePassIdx < entry.firstUsePassIdx) { // non-overlapping
                    lowestBinIdx = i;
                    break;
                }
            }
            if (lowestBinIdx == -1) {
                CreateNewBin(h, entry);
                continue;
            }
            auto& lb = bins[lowestBinIdx];
            lb.lastUsePassIdx = (std::max)(lb.lastUsePassIdx, entry.lastUsePassIdx);
            lb.occupants.emplace_back(h);
            lb.sizeInBytes = (std::max)(lb.sizeInBytes, entry.sizeInBytes);
        }
        /*
        walk bins in order, set each bin's offset to the running sum of previous bins' sizes, 
        aligned up to the heap-class alignment (64KB for textures via D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, 
        4KB for buffers via D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT if eligible; otherwise also 64KB).
        */
        uint64_t runningSumSize { 0 };
        uint64_t alignment = (hc == HC::UavBuffer) ? 4096u : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        for (size_t i = 0; i < bins.size(); ++i) {
            auto& bin = bins[i];
            runningSumSize = (runningSumSize + alignment - 1) & ~(alignment - 1);
            bin.offset = runningSumSize;
            for (auto& h : bin.occupants)
                mImpl->mTransientsPerFrame[mImpl->mCurrSlot].at(h).heapOffset = bin.offset; // actual update to each mTrasients.at(h).heapOffset
            runningSumSize += bin.sizeInBytes;
        }

        mImpl->mSolverHeapTotals[classIdx] = runningSumSize; // cache for dump

        // 2B-a : sanity check
        const uint64_t noAliasTotal = mImpl->mHeapHighWater[classIdx];
        if (runningSumSize > noAliasTotal) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "[FrameGraph] solver bug — class %d: solver wants %llu bytes, "
                "no-aliasing high-water was %llu. sum-of-max ≤ sum-of-all is supposed to hold.\n",
                classIdx,
                static_cast<unsigned long long>(runningSumSize),
                static_cast<unsigned long long>(noAliasTotal));
            OutputDebugStringA(msg);
            DebugBreak();
        }
        // 2B-b : obeservation (not bug checking)
        const size_t M = bins.size();
        const size_t N = hv.size();
        if (M < N && dumping) {
            char info[256];
            std::snprintf(info, sizeof(info),
                "[FrameGraph] class %d aliased: %zu bins covering %zu transients, "
                "%llu bytes (vs no-aliasing %llu bytes)\n",
                classIdx, M, N,
                static_cast<unsigned long long>(runningSumSize),
                static_cast<unsigned long long>(noAliasTotal));
            OutputDebugStringA(info);
        }
    }
}

// sink-callback shape is what makes the dump routable
void FrameGraph::DumpAliasingPlan(std::function<void(const char*)> sink) const {
    const auto& s = *mImpl;
    static const char* kClassNames[3] = { "RtvDsv", "UavBuffer", "TextureOnly" };

    auto FormatBytes = [](uint64_t bytes) -> std::string {
        char b[32];
        if (bytes >= (1ull << 20))
            std::snprintf(b, sizeof(b), "%llu MB",
                static_cast<unsigned long long>(bytes >> 20));
        else if (bytes >= (1ull << 10))
            std::snprintf(b, sizeof(b), "%llu KB",
                static_cast<unsigned long long>(bytes >> 10));
        else
            std::snprintf(b, sizeof(b), "%llu B",
                static_cast<unsigned long long>(bytes));
        return std::string(b);
        };

    std::string line;
    line.reserve(512);

    for (int c = 0; c < 3; ++c) {
        const auto& bins = s.mBinsPerClass[c];
        const uint64_t heapBytes = s.mSolverHeapTotals[c];

        // Count transients in this class for the header.
        size_t transientCount = 0;
        for (const auto& bin : bins) transientCount += bin.occupants.size();

        char header[256];
        std::snprintf(header, sizeof(header),
            "[%s] heap=%s, bins=%zu, transients=%zu\n",
            kClassNames[c],
            FormatBytes(heapBytes).c_str(),
            bins.size(),
            transientCount);
        sink(header);

        for (size_t bi = 0; bi < bins.size(); ++bi) {
            const auto& bin = bins[bi];

            line.clear();
            char binHeader[192];
            std::snprintf(binHeader, sizeof(binHeader),
                "  bin#%zu offset=0x%08llx size=%-8s last=%-3u occupants:",
                bi,
                static_cast<unsigned long long>(bin.offset),
                FormatBytes(bin.sizeInBytes).c_str(),
                bin.lastUsePassIdx);
            line.append(binHeader);

            for (size_t oi = 0; oi < bin.occupants.size(); ++oi) {
                FgResourceHandle h = bin.occupants[oi];
                const auto& entry = s.mTransientsPerFrame[mImpl->mCurrSlot].at(h);
                const char* name = entry.desc.debugName ? entry.desc.debugName : "<noname>";

                char occ[128];
                std::snprintf(occ, sizeof(occ),
                    "%s %s(%u..%u)",
                    oi == 0 ? "" : ",",
                    name,
                    entry.firstUsePassIdx,
                    entry.lastUsePassIdx);
                line.append(occ);
            }
            line.append("\n");
            sink(line.c_str());
        }
    }
    // dumping = true;
}

FrameGraph::FrameGraph(ID3D12Device* device) : mImpl(std::make_unique<Impl>()) {
    mImpl->mDevice = device;
}
FrameGraph::~FrameGraph() = default;

void FrameGraph::Begin(uint32_t slot) {
    assert(slot < gNumFrameResources);
    mImpl->mBlackboard.Clear();
    mImpl->mCurrSlot = slot;
    auto& s = *mImpl;
    s.mPassUsages.assign(mBorrowedPasses.size(), {});
    mCompiled.clear();

    // Transients are per-frame: passes re-Create them every Compile via Setup.
    // Without this reset they accumulate across frames, so sumPerClass grows
    // each frame and trips the high-water guard in SizeAndAllocateTransientHeaps.
    // The heaps and mHeapHighWater persist by design — only the per-frame
    // working state (entries, handle counter, aliasing bins) resets here.
    s.mTransientsPerFrame[slot].clear();
    s.mNextHandleIdPerSlot[slot] = 1;
    for (auto& bins : s.mBinsPerClass) bins.clear();
}

void FrameGraph::End() {
    // Free this frame's transient descriptors. QueueRemoval results in removal of desc heap when this `frame` gets its turn back to render
    auto& s = *mImpl;
    auto& dhm = DescHeapManager::Get();
    for (auto& [h, entry] : s.mTransientsPerFrame[s.mCurrSlot]) {
        if (!entry.resource) continue;
        if (entry.desc.kind == FgResourceDesc::Kind::Buffer)
            dhm.QueueRemoval_Buffer(static_cast<MCBufferResource&>(*entry.resource));
        else
            dhm.QueueRemoval_Texture(static_cast<MCTextureResource&>(*entry.resource));
    }
}

// create a DAG using RAW/WAW dependencies
// then produce a topological ordering of DAG (Kahn's algorithm)
// read-only against engine
void FrameGraph::Compile(BarrierManager& bm, CpuTimer& cpuTimer) {
    CPU_SCOPED(cpuTimer, "FrameGraph::Compile");
    auto& s = *mImpl;
    const uint32_t N = static_cast<uint32_t>(mBorrowedPasses.size());
    // === A. Setup phase ===
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][A] Setup");
        s.mPassUsages.assign(N, {}); // N: count, {}: value
        for (uint32_t i = 0; i < N; ++i) {
            RenderPassBuilder b(this, i);
            mBorrowedPasses[i]->Setup(b);
        }
    }



    // === B. Cull ===
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][B] Compute Live");
        ComputeLiveSet(); // populates mImpl->mLiveSets
    }
    const uint32_t liveCount = static_cast<uint32_t>(
        std::count(mImpl->mLiveSet.begin(), mImpl->mLiveSet.end(), true));// vector<bool>, count number of true elements

    // === C. Dependency graph building: group live passes by the resources they touch ===
    auto ResolveUsageResource = [&](const Impl::ResourceUsage& u) -> MCResource* {
        if (std::holds_alternative<MCResource*>(u.resource))
            return std::get<MCResource*>(u.resource);
        FgResourceHandle h = std::get<FgResourceHandle>(u.resource);
        auto it = s.mTransientsPerFrame[s.mCurrSlot].find(h);
        if (it == s.mTransientsPerFrame[s.mCurrSlot].end()) {
            OutputDebugStringA("[FG] usage references unknown transient handle\n");
            DebugBreak();
            return nullptr;
        }
        return it->second.resource.get();
        };
    std::unordered_map<MCResource*, std::vector<uint32_t>> writers;
    std::unordered_map<MCResource*, std::vector<uint32_t>> readers;

    std::vector<std::vector<uint32_t>> adj(N);// adj[i] = {j, k} : pass i depends on pass j and k - therefore pass j and k must run before pass i
    std::vector<uint32_t> inDegree(N, 0); // inDegree[i] : # of passes pass_i depends on
    std::vector<std::unordered_set<uint32_t>> seen(N); // per-source dedup set

    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;

    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][C] DependencyGraph");
        for (uint32_t i = 0; i < N; ++i) {
            if (!mImpl->mLiveSet[i]) continue; // cull non-live
            for (const auto& u : s.mPassUsages[i]) {
                // D1 Step 0e: ResourceUsage::resource is std::variant<MCResource*, FgResourceHandle>.
                // For D1, every site is import-only — std::get is safe until transients enter at 0f.
                MCResource* res = ResolveUsageResource(u);
                (u.isWrite ? writers : readers)[res].push_back(i);
            }
        }

        auto addEdge = [&](uint32_t from, uint32_t to) { // captures all four containers by reference
            if (from == to) return; // degenerate case (write & read from same source within pass)
            if (seen[from].insert(to).second) { // `.insert(TO).second` returns false if TO already exists (duplicate)
                adj[from].push_back(to);
                ++inDegree[to];
            } // only true insertion mutates adj and inDegree
            };

        // RAW: every writer of R precedes every reader of R. (Read After Write)
        for (auto& [res, ws] : writers) {
            auto rit = readers.find(res);
            if (rit == readers.end()) continue; // no readers for this resource; skip
            for (uint32_t w : ws)
                for (uint32_t r : rit->second)
                    addEdge(w, r); // every writer w must precede every reader r
        }

        // WAW: earlier-registered writer precedes later-registered writer of the same R.
        for (auto& [res, ws] : writers) {
            for (size_t i = 0; i < ws.size(); ++i)
                for (size_t j = 0; j < ws.size(); ++j)
                    if (ws[i] < ws[j]) addEdge(ws[i], ws[j]);
        }
    }

    // topoOrder contains every pass that participated in the sort, 
    // in dependency-respecting order with registration-order tiebreak. 
    std::vector<uint32_t> topoOrder;
    topoOrder.reserve(N);

    // === D. Kahn's algorithm with min-heap tiebreak ===
    // std::greater -- flips priority_queue ordering to min-heap from max-heap
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][D] Kahns' algorithm");
        for (uint32_t i = 0; i < N; ++i) {
            if (!mImpl->mLiveSet[i]) continue; // cull non-live
            if (inDegree[i] == 0) ready.push(i);
        }
        // classic Kahn's loop (topoOrder loop)
        while (!ready.empty()) {
            uint32_t i = ready.top(); // min-heap, so top() returns smallest index
            ready.pop();
            topoOrder.push_back(i);
            for (uint32_t j : adj[i])
                if (--inDegree[j] == 0) ready.push(j);
        }

        // After Kahn's terminates, any pass with inDegree > 0 is
        // part of a cycle (or downstream of one) — it never became ready. 
        // The error message names each such pass using TypeName() 
        if (topoOrder.size() != liveCount) {
            std::string msg = "FrameGraph::Compile: cycle involving:";
            for (uint32_t i = 0; i < N; ++i) {
                if (!mImpl->mLiveSet[i]) continue; // cull non-live
                if (inDegree[i] > 0) { msg += "\n  - "; msg += mBorrowedPasses[i]->TypeName(); }
            }
            throw std::runtime_error(msg);
        }
    }
    // E. Compute transient lifetimes
    std::vector<uint32_t> execOrder(N);
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][E] Compute Transient lifetime");
        for (uint32_t e = 0; e < topoOrder.size(); ++e)
            execOrder[topoOrder[e]] = e;
        ComputeTransientLifetimes(execOrder);
    }
    // F. Transient heap sizing ===
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][F] Transient Heap size determined & created");
        SizeAndAllocateTransientHeaps();
    }
    // G. Aliasing
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][G] SolveAliasing-bin assignment");
        SolveAliasing();
    }
    // H. specifying which resource gets aliased to which resource at which pass
    std::vector<std::vector<CompiledPass::AliasBarrier>> perPassAliasBarriers(N);
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][H] perPassAliasBarrier setup");
        for (int c = 0; c < 3; ++c) {
            for (const auto& bin : mImpl->mBinsPerClass[c]) {
                for (size_t k = 1; k < bin.occupants.size(); ++k) {     // start from 1 — index 0 has no predecessor
                    FgResourceHandle thisH = bin.occupants[k];
                    FgResourceHandle prevH = bin.occupants[k - 1];

                    const auto& thisEntry = mImpl->mTransientsPerFrame[s.mCurrSlot].at(thisH);
                    const auto& prevEntry = mImpl->mTransientsPerFrame[s.mCurrSlot].at(prevH);

                    perPassAliasBarriers[thisEntry.firstUsePassIdx].push_back({
                        prevEntry.resource.get(),
                        thisEntry.resource.get()
                        });

                    // sanity check - first use of transient must be Write
                    auto IsWriteState = [](D3D12_RESOURCE_STATES s) {
                        return (s & (D3D12_RESOURCE_STATE_RENDER_TARGET
                            | D3D12_RESOURCE_STATE_DEPTH_WRITE
                            | D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                            | D3D12_RESOURCE_STATE_COPY_DEST)) != 0;
                        };

                    const uint32_t firstUseRegIdx = topoOrder[thisEntry.firstUsePassIdx];
                    D3D12_RESOURCE_STATES firstUseState = D3D12_RESOURCE_STATE_COMMON;
                    for (const auto& u : mImpl->mPassUsages[firstUseRegIdx]) {
                        if (!std::holds_alternative<FgResourceHandle>(u.resource)) continue;
                        if (std::get<FgResourceHandle>(u.resource) != thisH) continue;
                        firstUseState = u.required;
                        break;
                    }

                    if (!IsWriteState(firstUseState)) {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                            "[FrameGraph] aliased transient %s (handle=%u) is read before first "
                            "write at pass %u (exec %u); UB. Either move the aliasing or change "
                            "the first use to a write.\n",
                            thisEntry.desc.debugName ? thisEntry.desc.debugName : "<noname>",
                            thisH.id,
                            firstUseRegIdx,
                            thisEntry.firstUsePassIdx);
                        OutputDebugStringA(buf);
                        DebugBreak();
                    }
                }
            }
        }
    }

    // I - Allocation
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][I] CreatePlacedResource-Allocation");
        CreatePlacedResources();
    }
    
    // J. Transition list ===
    std::unordered_map<MCResource*, D3D12_RESOURCE_STATES> simState;
    std::vector<std::vector<CompiledPass::Transition>> perPassTransitions(N);
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][J] StateTransitionList at pass entry");
        for (uint32_t i = 0; i < N; ++i) { // for each pass
            if (!mImpl->mLiveSet[i]) continue; // cull non-live
            for (const auto& u : s.mPassUsages[i]) { // for each ResourceUsage
                MCResource* res = ResolveUsageResource(u);
                if (simState.find(res) == simState.end()) // if not registered
                    simState[res] = bm.GetCurrentState(*res); // register state
            }
        }

        // perPassTransitions[i] accumulates the barriers pass_i will need at its entry
        for (uint32_t i : topoOrder) { // for each pass (pass_i)
            for (const auto& u : s.mPassUsages[i]) { // for each resourceUsage of pass_i
                MCResource* res = ResolveUsageResource(u);
                auto& cur = simState[res];
                if (cur != u.required) {
                    perPassTransitions[i].push_back({ res, u.required });
                    cur = u.required;
                }
            }
        }
    }

    // K. Materialize mCompiled ===
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][K] Materialize mCompiled");
        mCompiled.clear();
        mCompiled.reserve(N);

        for (uint32_t execIdx = 0; execIdx < liveCount; ++execIdx) {
            uint32_t regIdx = topoOrder[execIdx];
            mCompiled.push_back({
                mBorrowedPasses[regIdx],
                std::move(perPassTransitions[regIdx]),
                std::move(perPassAliasBarriers[execIdx]),    // interpret as exec-order index
                });
        }
    }

    // L. commit transient descriptors created in Compile.
    {
        CPU_SCOPED(cpuTimer, "[FG::Compile][L] commit transient desc to shader visible");
        DescHeapManager::Get().CommitToShaderVisible(); // dirty[] flags scope copy to new DYNAMIC tier only; static is untouched here
    }
    if (dumping) dumping = false;
}

void FrameGraph::Execute(ID3D12GraphicsCommandList* cmdList, BarrierManager& bm, GpuTimer& timer) {
    for (auto& cp : mCompiled) {
        for (auto& a : cp.aliasBarriers) {
            bm.AliasBarrier(*a.before, *a.after);
        }
        for (auto& t : cp.transitions) bm.TransitionState(*t.resource, t.targetState);
        bm.FlushBarriers(cmdList);
        
        // PIXScopedEvent(cmdList, 0, cp.pass->TypeName());
        if (cp.pass->mTimerId == TimerID(-1))
            cp.pass->mTimerId = timer.Register(cp.pass->TypeName());
        GpuTimer::Scoped scope(cmdList, timer, cp.pass->mTimerId, cp.pass->TypeName());
        FgRenderContext rc;
        rc.cmdList = cmdList;
        rc.graph = this;
        cp.pass->Execute(rc);
    }
}

void FrameGraph::RegisterPass(RenderPass* pass) {
    assert(pass != nullptr && "FrameGraph::RegisterPass: null pass");
    assert(std::find(mBorrowedPasses.begin(), mBorrowedPasses.end(), pass) == mBorrowedPasses.end()
        && "FrameGraph::RegisterPass: pass already registered");
    mBorrowedPasses.push_back(pass);
}

void FrameGraph::ComputeLiveSet() {
    const uint32_t N = static_cast<uint32_t>(mBorrowedPasses.size());
    mImpl->mLiveSet.assign(N, false);
    // 1. marking passes writing to root resource as 'live' pass.
    for (uint32_t p = 0; p < N; ++p) {
        for (const auto& u : mImpl->mPassUsages[p]) {
            if (!u.isWrite) continue;
            if (!std::holds_alternative<MCResource*>(u.resource)) continue;
            MCResource* w = std::get<MCResource*>(u.resource);
            for (MCResource* root : mImpl->mRoots) {
                if (w == root) {
                    mImpl->mLiveSet[p] = true;
                    break;
                }
            }
        }
    }
    // 2. transitive closure.
    // every pass writing to a resource that the live pass reads become live.
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t p = 0; p < N;++p) {
            if (!mImpl->mLiveSet[p]) continue;
            for (const auto& u : mImpl->mPassUsages[p]) {
                if (u.isWrite) continue;
                // find producers of u.resource across all passes
                for (uint32_t q = 0; q < N; ++q) {
                    if (mImpl->mLiveSet[q]) continue; // already live
                    for (const auto& producer : mImpl->mPassUsages[q]) {
                        if (!producer.isWrite) continue;
                        if (producer.resource != u.resource) continue;
                        mImpl->mLiveSet[q] = true;
                        changed = true;
                        break;
                    }
                }
            }
        }
    }
}

// helper called by RenderPassBuilder
void FrameGraph::RecordUsage(uint32_t passIdx, MCResource* res, D3D12_RESOURCE_STATES req, bool isWrite) {
    mImpl->mPassUsages[passIdx].push_back({ passIdx, res, req, isWrite });
}

// handle overload — for transient-handle reads/writes from Setup
void FrameGraph::RecordUsage(uint32_t passIdx, FgResourceHandle h, D3D12_RESOURCE_STATES req, bool isWrite) {
    mImpl->mPassUsages[passIdx].push_back({ passIdx, h, req, isWrite });
}


void FrameGraph::MarkRoot(MCResource& res) {
    mImpl->mRoots.push_back(&res);
}

void FrameGraph::DumpToOutputDebugString() const {
    auto stateName = [](D3D12_RESOURCE_STATES s) -> const char* {
        switch (s) {
        case D3D12_RESOURCE_STATE_COMMON:                    return "COMMON";
        case D3D12_RESOURCE_STATE_RENDER_TARGET:             return "RENDER_TARGET";
        case D3D12_RESOURCE_STATE_DEPTH_WRITE:               return "DEPTH_WRITE";
        case D3D12_RESOURCE_STATE_DEPTH_READ:                return "DEPTH_READ";
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:     return "PIXEL_SHADER_RESOURCE";
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE: return "NON_PIXEL_SHADER_RESOURCE";
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:          return "UNORDERED_ACCESS";
        case D3D12_RESOURCE_STATE_COPY_DEST:                 return "COPY_DEST";
        case D3D12_RESOURCE_STATE_COPY_SOURCE:               return "COPY_SOURCE";
        default:                                             return "<multi-bit>";
        }
        };

    auto resName = [](const MCResource* r) -> std::string {
        return (r && !r->Name.empty()) ? r->Name : std::string("<unnamed>");
        };

    const auto& s = *mImpl;

    // Header line.
    {
        std::string line = "[FG] === FrameGraph dump: " +
            std::to_string(mBorrowedPasses.size()) + " passes registered, " +
            std::to_string(mCompiled.size()) + " compiled ===\n";
        OutputDebugStringA(line.c_str());
    }

    const uint32_t liveCount = static_cast<uint32_t>(
        std::count(mImpl->mLiveSet.begin(), mImpl->mLiveSet.end(), true));// vector<bool>, count number of true elements
    std::string liveCountLine = "[FG] liveCount: " + std::to_string(liveCount) + "\n";
    OutputDebugStringA(liveCountLine.c_str());

    // Per-pass entries in registration order.
    for (size_t i = 0; i < mBorrowedPasses.size(); ++i) {
        // Find this pass's position in topological order (== mCompiled index).
        // O(N) per pass, O(N^2) total. Fine for N <= 10.
        size_t execIdx = mCompiled.size();   // sentinel: "not in mCompiled"
        for (size_t j = 0; j < mCompiled.size(); ++j) {
            if (mCompiled[j].pass == mBorrowedPasses[i]) { execIdx = j; break; }
        }
        const bool compiled = (execIdx < mCompiled.size());
        const CompiledPass* cp = compiled ? &mCompiled[execIdx] : nullptr;

        // Count reads/writes from Setup declarations.
        size_t reads = 0, writes = 0;
        if (i < s.mPassUsages.size()) {
            for (const auto& u : s.mPassUsages[i]) {
                (u.isWrite ? writes : reads) += 1;
            }
        }

        // Header line for this pass.
        std::string header = "[FG] Pass " + std::to_string(i) + ": " +
            mBorrowedPasses[i]->TypeName() + " | ";
        if (compiled) header += "exec: " + std::to_string(execIdx);
        else          header += "exec: (not compiled / culled)";
        header += " | reads: " + std::to_string(reads);
        header += " | writes: " + std::to_string(writes);
        header += " | transitions: " + std::to_string(cp ? cp->transitions.size() : 0);
        header += "\n";
        OutputDebugStringA(header.c_str());

        // Per-usage detail lines (in declaration order — order Setup recorded them).
        if (i < s.mPassUsages.size()) {
            for (const auto& u : s.mPassUsages[i]) {
                std::string line = "[FG]   ";
                line += (u.isWrite ? "W " : "R ");
                line += resName(std::get<MCResource*>(u.resource));
                line += " : ";
                line += stateName(u.required);
                line += "\n";
                OutputDebugStringA(line.c_str());
            }
        }

        // Transitions emitted before this pass's Execute (from the compiled plan).
        if (cp) {
            for (const auto& t : cp->transitions) {
                std::string line = "[FG]   transition: " +
                    resName(t.resource) + " -> " +
                    stateName(t.targetState) + "\n";
                OutputDebugStringA(line.c_str());
            }
        }
    }
    dumping = true;
}

FgBlackboard& FrameGraph::Blackboard() { return mImpl->mBlackboard; }
const FgBlackboard& FrameGraph::Blackboard() const { return mImpl->mBlackboard; }

FrameGraph::FgPanelData FrameGraph::BuildPanelSnapshot(const GpuTimer& timer) const {
    /*
    borrows that function's mBorrowedPasses-walk verbatim, returning data instead of stringifying. 
    aliasOver strings, if kept, point at the held TransientEntry::desc.debugName 
    (string-literal lifetime, D1 deviation #8) — safe to store as const char*.
    */
    const auto& s = *mImpl;
    const auto& transients = s.mTransientsPerFrame[s.mCurrSlot];

    FgPanelData out{};

    // --- Scalar totals. Both already cached; do NOT recompute. ---
    //   mSolverHeapTotals[c] : post-alias per-class total  (SolveAliasing, :333)
    //   mHeapHighWater[c]    : sum-of-all aligned sizes     (SizeAndAllocateTransientHeaps, :175)
    for (int c = 0; c < 3; ++c) {
        out.bytesAliased += s.mSolverHeapTotals[c];
        out.bytesNoAlias += s.mHeapHighWater[c];
    }
    out.liveCount = static_cast<uint32_t>(std::count(s.mLiveSet.begin(), s.mLiveSet.end(), true));
    out.culledCount = static_cast<uint32_t>(s.mLiveSet.size()) - out.liveCount;
    out.compileMs = -1.f;   // Step 3.1 swaps this for the std::chrono rolling avg.

    // --- Alias annotations: hang each successor's predecessor-name on the
    //     registered pass that WRITES the successor (its producer row). ---
    std::unordered_map<uint32_t, const char*> aliasByPass;  // regPassIdx -> predecessor debugName
    for (int c = 0; c < 3; ++c) {
        for (const auto& bin : s.mBinsPerClass[c]) {
            for (size_t k = 1; k < bin.occupants.size(); ++k) {
                const FgResourceHandle here = bin.occupants[k];
                const FgResourceHandle prev = bin.occupants[k - 1];

                const auto pit = transients.find(prev);
                const char* prevName =
                    (pit != transients.end() && pit->second.desc.debugName)
                    ? pit->second.desc.debugName : "<unnamed>";

                // First registered pass that writes `here` owns the alias barrier.
                for (uint32_t p = 0; p < static_cast<uint32_t>(s.mPassUsages.size()); ++p) {
                    bool writesHere = false;
                    for (const auto& u : s.mPassUsages[p]) {
                        if (!u.isWrite) continue;
                        if (!std::holds_alternative<FgResourceHandle>(u.resource)) continue;
                        if (std::get<FgResourceHandle>(u.resource) == here) { writesHere = true; break; }
                    }
                    if (writesHere) { aliasByPass[p] = prevName; break; }
                }
            }
        }
    }

    // --- Per-pass rows, registration order, full set (live + culled). ---
    //     mBorrowedPasses is a FrameGraph member (not Impl); mLiveSet / mPassUsages are reg-indexed.
    out.rows.reserve(mBorrowedPasses.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(mBorrowedPasses.size()); ++i) {
        FgPanelRow row{};
        row.name = mBorrowedPasses[i]->TypeName();
        row.culled = (i < s.mLiveSet.size()) ? !s.mLiveSet[i] : true;

        uint32_t reads = 0, writes = 0;
        if (i < s.mPassUsages.size())
            for (const auto& u : s.mPassUsages[i]) (u.isWrite ? writes : reads) += 1u;
        row.reads = reads;
        row.writes = writes;

        const auto ait = aliasByPass.find(i);
        row.aliasOver = (ait != aliasByPass.end()) ? ait->second : nullptr;

        // Culled this frame -> '—' even if a prior frame populated mTimerId.
        const TimerID tid = mBorrowedPasses[i]->mTimerId;
        row.gpuMs = (row.culled || tid == TimerID(-1)) ? -1.f : timer.GetAverageMs(tid);

        out.rows.push_back(row);
    }
    return out;
}

// ---------------------- RenderPassBuilder ----------------------------

RenderPassBuilder::RenderPassBuilder(FrameGraph* g, uint32_t pi) : mGraph(g), mPassIdx(pi) {}

void RenderPassBuilder::Read(MCResource& res, D3D12_RESOURCE_STATES req) {
    mGraph->RecordUsage(mPassIdx, &res, req, /*isWrite=*/false);
}

void RenderPassBuilder::Write(MCResource& res, D3D12_RESOURCE_STATES req) {
    mGraph->RecordUsage(mPassIdx, &res, req, /*isWrite=*/true);
}

void RenderPassBuilder::Read(FgResourceHandle h, D3D12_RESOURCE_STATES req) {
    mGraph->RecordUsage(mPassIdx, h, req, /*isWrite=*/false);
}

void RenderPassBuilder::Write(FgResourceHandle h, D3D12_RESOURCE_STATES req) {
    mGraph->RecordUsage(mPassIdx, h, req, /*isWrite=*/true);
}

FgResourceHandle RenderPassBuilder::Create(const FgResourceDesc& desc) {
    return mGraph->CreateTransient(desc);
}

// ---------------------- FgRenderContext ----------------------------
uint32_t FgRenderContext::GetSRV(FgResourceHandle h) const {
    auto& impl = *graph->mImpl;
    auto it = impl.mTransientsPerFrame[impl.mCurrSlot].find(h);
    assert(it != impl.mTransientsPerFrame[impl.mCurrSlot].end() && it->second.resource && "unknown transient handle");
    auto& e = it->second;
    // SRVs/UAVs are on the derived types, not MCResource. CreateTransient chose
    // the concrete type by desc.kind, so the matching cast is the one evaluated.
    auto& srvs = (e.desc.kind == FgResourceDesc::Kind::Buffer)
        ? static_cast<MCBufferResource&>(*e.resource).SRVs
        : static_cast<MCTextureResource&>(*e.resource).SRVs;
    assert(!srvs.empty() && "transient has no SRV");
    return static_cast<uint32_t>(srvs[0].offset);
}

uint32_t FgRenderContext::GetUAV(FgResourceHandle h) const {
    auto& impl = *graph->mImpl;
    auto it = impl.mTransientsPerFrame[impl.mCurrSlot].find(h);
    assert(it != impl.mTransientsPerFrame[impl.mCurrSlot].end() && it->second.resource && "unknown transient handle");
    auto& e = it->second;
    // SRVs/UAVs are on the derived types, not MCResource. CreateTransient chose
    // the concrete type by desc.kind, so the matching cast is the one evaluated.
    auto& uavs = (e.desc.kind == FgResourceDesc::Kind::Buffer)
        ? static_cast<MCBufferResource&>(*e.resource).UAVs
        : static_cast<MCTextureResource&>(*e.resource).UAVs;
    assert(!uavs.empty() && "transient has no SRV");
    return static_cast<uint32_t>(uavs[0].offset);
}

ID3D12Resource* FgRenderContext::GetResource(FgResourceHandle h) const {
    if (!h.IsValid()) return nullptr;
    auto& impl = *graph->mImpl;
    auto it = impl.mTransientsPerFrame[impl.mCurrSlot].find(h);
    if (it == impl.mTransientsPerFrame[impl.mCurrSlot].end()) return nullptr;
    return it->second.resource->mResource.Get();
}

MCResource& FgRenderContext::GetMCResource(FgResourceHandle h) const {
    assert(h.IsValid() && "FgRenderContext::GetMCResource: invalid handle");
    auto& impl = *graph->mImpl;
    auto it = impl.mTransientsPerFrame[impl.mCurrSlot].find(h);
    assert(it != impl.mTransientsPerFrame[impl.mCurrSlot].end() && "FgRenderContext::GetMCResource: unknown handle");
    return *it->second.resource;
}