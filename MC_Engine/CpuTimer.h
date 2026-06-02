#pragma once
#include <chrono>
#include "ConstExpressionValues.h"
#include <unordered_map>
#include <vector>
#include <array>
#include <cstdint>
#include <string>

using CpuTimerID = uint32_t;

class CpuTimer
{
public:
    CpuTimerID Register(const char* name) {
        auto it = mNameToId.find(name);
        if (it != mNameToId.end()) return it->second;
        const CpuTimerID id = static_cast<CpuTimerID>(mNames.size());
        mNameToId.emplace(name, id);
        mNames.emplace_back(name);
        mSamples.emplace_back();      // std::array<float,kRingSize> value-inits to 0
        mRingHead.push_back(0);
        mFilled.push_back(0);
        return id;
    }
    void Record(CpuTimerID id, float ms) {
        mSamples[id][mRingHead[id]] = ms;
        mRingHead[id] = (mRingHead[id] + 1) % kRingSize;
        if (mFilled[id] < kRingSize) ++mFilled[id];
    }
    float GetAverageMs(CpuTimerID id, int windowSamples = 60) const {
        if (id >= mSamples.size() || mFilled[id] == 0) return 0.f;
        const int n = (std::min)(windowSamples, mFilled[id]);
        const auto& ring = mSamples[id];
        const int head = mRingHead[id];                 // next write slot
        float sum = 0.f;
        for (int k = 1; k <= n; ++k)
            sum += ring[(head - k + kRingSize) % kRingSize];
        return sum / static_cast<float>(n);
    }
    float GetAverageMs(const char* name, int windowSamples = 60) const {
        auto it = mNameToId.find(name);
        return (it == mNameToId.end()) ? 0.f : GetAverageMs(it->second, windowSamples);
    }
    int                RegisteredCount() const { return (int)mNames.size(); }
    const std::string& Name(CpuTimerID id) const { return mNames.at(id); }
    const float* SamplesData(CpuTimerID id) const { return mSamples[id].data(); }
    int                SamplesCount() const { return kRingSize; }
    int                SamplesHead(CpuTimerID id) const { return mRingHead[id]; }


    class Scoped {
    public:
        Scoped(CpuTimer& timer, CpuTimerID id) : mTimer(timer), mId(id),
            mStart(std::chrono::high_resolution_clock::now()) {}
        ~Scoped() {
            const auto end = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - mStart).count();
            mTimer.Record(mId, static_cast<float>(ms));
        }
        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;
    private:
        CpuTimer& mTimer;
        CpuTimerID mId;
        std::chrono::high_resolution_clock::time_point mStart;
    };
private:
    static constexpr int kRingSize = 120;   // same window depth as GpuTimer (GpuTimer.h:62)
    std::unordered_map<std::string, CpuTimerID> mNameToId;
    std::vector<std::string>                    mNames;     // id -> name
    std::vector<std::array<float, kRingSize>>   mSamples;   // id -> ring
    std::vector<int>                            mRingHead;  // id -> next write slot
    std::vector<int>                            mFilled;    // id -> valid count (caps at kRingSize)

};

// Token-pasting indirection so __LINE__ expands before paste — same idiom as
// GPU_SCOPED (GpuTimer.h:68-86). Unlike GPU_SCOPED, the timer is an explicit
// argument, so CPU_SCOPED works anywhere a CpuTimer& is in scope, not only inside
// an MCEngine member.
#define CPU_SCOPED_CONCAT_INNER(a, b) a##b
#define CPU_SCOPED_CONCAT(a, b)       CPU_SCOPED_CONCAT_INNER(a, b)

// Usage: { CPU_SCOPED(mCpuTimer, "FrameGraph::Compile"); /* work */ }
#define CPU_SCOPED(timer, name) \
    static CpuTimerID CPU_SCOPED_CONCAT(_cpu_id_, __LINE__) = (timer).Register(name); \
    CpuTimer::Scoped  CPU_SCOPED_CONCAT(_cpu_scope_, __LINE__)( \
        (timer), CPU_SCOPED_CONCAT(_cpu_id_, __LINE__))