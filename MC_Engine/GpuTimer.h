#pragma once
#include "ConstExpressionValues.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

class MCEngine;
using TimerID = uint32_t;

class GpuTimer {
public:
    explicit GpuTimer(MCEngine& engine);
    void Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue);
	TimerID Register(const char* name);
    void    BeginTimer(ID3D12GraphicsCommandList* cmdList, TimerID id);
    void    EndTimer(ID3D12GraphicsCommandList* cmdList, TimerID id);
    void    ResolveForFrame(ID3D12GraphicsCommandList* cmdList); // same as EndFrame()
    void    ReadbackForFrame();
    float   GetAverageMs(TimerID id, int windowFrames = 60) const;
    void    BeginFrame();
    void    Reset();

    // Diagnostics.
    int                RegisteredCount() const { return (int)mNames.size(); }
    const std::string& Name(TimerID id) const { return mNames.at(id); }

    // Capacity. 32 named timers × 2 slots = 64. Bump if you exceed.
    static constexpr UINT kMaxSlots = 64;
    
    // for imgui's sparkline data visualization
    const float* SamplesData(TimerID id) const { return mSamples[id].data(); }
    int          SamplesCount() const { return kRingSize; }
    int          SamplesHead(TimerID id) const { return mRingHead[id]; }

    class Scoped {
    public:
        Scoped(ID3D12GraphicsCommandList* cmdList, GpuTimer& timer, TimerID id, const char* name);
        ~Scoped();

        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;

    private:
        ID3D12GraphicsCommandList* mCmdList;
        GpuTimer&                  mTimer;
        TimerID                    mId;
    };

private:
    MCEngine& mEngine;
    UINT64    mTimestampFreq = 0;        // cached at construction.
    std::array<Microsoft::WRL::ComPtr<ID3D12QueryHeap>, gNumFrameResources> mHeaps;       // gNumFrameResources = 3
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, gNumFrameResources> mReadbacks;
    std::array<std::vector<bool>, gNumFrameResources> mFiredPerFrame;   // Outer index = frame slot (0..gNumFrameResources-1). Inner = timer id.

    std::unordered_map<std::string, TimerID> mNameToId;
    std::vector<std::string>                 mNames; // Instead of IdToNames, just use std::vector

    static constexpr int kRingSize = 120; // for taking average among 120 samples per registered timer.
    std::vector<std::array<float, kRingSize>> mSamples; // TimerID maps here as index
    std::vector<int>                          mRingHead; // TimerID maps here as index. 
    // mRingHead : among 120 rings (of that sample), which to point at.
};

// Token-pasting helpers — __LINE__ needs two-level indirection to expand
// before paste. Standard preprocessor idiom.
#define GPU_SCOPED_CONCAT_INNER(a, b) a##b
#define GPU_SCOPED_CONCAT(a, b)       GPU_SCOPED_CONCAT_INNER(a, b)

// Usage: GPU_SCOPED(mCommandList.Get(), "Opaque");
// Effects: opens a PIX event labeled `name`, then issues the GPU timer's
// Begin EndQuery. The destructor at scope-end issues End EndQuery and closes
// the PIX event. The single string literal serves as both PIX label and
// timer registry key.
// Requires: caller is inside an MCEngine member function (so `mGpuTimer`
// is in scope). Static TimerID per call site means Register() runs exactly
// once per site — the second time at the same site reuses the cached ID,
// which is consistent with Register's throw-on-duplicate (different sites
// must use different names; the static is initialized once per site).
#define GPU_SCOPED(cmdList, name) \
    static TimerID GPU_SCOPED_CONCAT(_gpu_id_, __LINE__) = mGpuTimer.Register(name); \
    GpuTimer::Scoped GPU_SCOPED_CONCAT(_gpu_scope_, __LINE__)( \
        (cmdList), mGpuTimer, GPU_SCOPED_CONCAT(_gpu_id_, __LINE__), (name))