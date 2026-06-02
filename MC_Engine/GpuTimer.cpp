#define NOMINMAX
#include "GpuTimer.h"
#include "MCEngine.h"
#include "Common/d3dUtil.h"   // ThrowIfFailed
#include <cmath>
#include <stdexcept>
#include <d3dx12.h>

#include <pix3.h>
using Microsoft::WRL::ComPtr;

GpuTimer::GpuTimer(MCEngine& engine) : mEngine(engine) {}

void GpuTimer::Reset() {
    /*
    for (auto& i : mHeaps) 
        i.Reset();
    for (auto& i : mReadbacks)
        i.Reset();
    */

    // this is enough
    mHeaps = {};
    mReadbacks = {};
}

void GpuTimer::Initialize(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue)
{
	if( FAILED(graphicsQueue->GetTimestampFrequency(&mTimestampFreq)))
		throw std::runtime_error("GpuTimer: GetTimestampFrequency failed");

    D3D12_QUERY_HEAP_DESC qhDesc{};
    qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.Count = kMaxSlots;
    qhDesc.NodeMask = 0;

    const UINT64 readbackBytes = sizeof(UINT64) * kMaxSlots;
    const auto rbHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto rbBufDesc = CD3DX12_RESOURCE_DESC::Buffer(readbackBytes);

    // mHeaps.size() == gNumFrameResources (defined with gNumFrameResources in GpuTimer.h)
    for (UINT i = 0; i < (UINT)mHeaps.size(); ++i) {
        ThrowIfFailed(device->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&mHeaps[i])));
        ThrowIfFailed(device->CreateCommittedResource(
            &rbHeapProps, D3D12_HEAP_FLAG_NONE,
            &rbBufDesc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&mReadbacks[i])));
    }
}

TimerID GpuTimer::Register(const char* name) {
    auto it = mNameToId.find(name);
    if (it != mNameToId.end()) return it->second;
    assert(mNames.size() * 2 + 2 <= kMaxSlots
        && "GpuTimer: kMaxSlots exceeded; bump it in GpuTimer.h.");
    TimerID id = static_cast<TimerID>(mNames.size());
    mNames.emplace_back(name);
    mNameToId.emplace(mNames.back(), id);
    mSamples.emplace_back();
    mRingHead.emplace_back(0);
    // Keep every per-frame flag vector consistent with mNames immediately —
    // so first-frame lazy registration in FrameGraph::Execute can't outrun
    // the next BeginFrame's resize.
    for (auto& flags : mFiredPerFrame) {
        if (flags.size() < mNames.size()) flags.resize(mNames.size(), false);
    }
    return id;
}

void GpuTimer::BeginFrame() {
    const int frame = mEngine.GetCurrFrameResourceIndex();
    auto& flags = mFiredPerFrame[frame];
    if (flags.size() != mNames.size()) flags.resize(mNames.size(), false);
    std::fill(flags.begin(), flags.end(), false);
}

void GpuTimer::BeginTimer(ID3D12GraphicsCommandList* cmdList, TimerID id) {
    const int frame = mEngine.GetCurrFrameResourceIndex();
    cmdList->EndQuery(mHeaps[frame].Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        id * 2);
}

void GpuTimer::EndTimer(ID3D12GraphicsCommandList* cmdList, TimerID id) {
    const int frame = mEngine.GetCurrFrameResourceIndex();
    cmdList->EndQuery(mHeaps[frame].Get(),
        D3D12_QUERY_TYPE_TIMESTAMP, 
        id * 2 + 1);
    mFiredPerFrame[frame][id] = true;
}

void GpuTimer::ResolveForFrame(ID3D12GraphicsCommandList* cmdList) {
    if (mNames.empty()) return;
    const int frame = mEngine.GetCurrFrameResourceIndex();
    const auto& flags = mFiredPerFrame[frame];
    for (size_t i = 0; i < mNames.size(); i++) {
        if(!flags[i]) continue;
        cmdList->ResolveQueryData(
            mHeaps[frame].Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            static_cast<UINT>(i * 2), 
            2,
            mReadbacks[frame].Get(),
            i * 2 * sizeof(UINT64));
    }
}

void GpuTimer::ReadbackForFrame() {
    if (mNames.empty()) return;
    const int frame = mEngine.GetCurrFrameResourceIndex();
    const auto& flags = mFiredPerFrame[frame];
    auto* rb = mReadbacks[frame].Get();
    UINT64* base = nullptr;
    const D3D12_RANGE readRange{ 0, sizeof(UINT64) * 2 * mNames.size() };
    if (FAILED(rb->Map(0, &readRange, reinterpret_cast<void**>(&base))))
        return;
    for (size_t i = 0; i < mNames.size(); i++) {
        if (!flags[i]) continue;
        const UINT64 start = base[i * 2];
        const UINT64 end = base[i * 2 + 1];
        // mSamples[i][mRingHead[i]] = static_cast<float>((double)(end - start));
        const float ms = static_cast<float>((double)(end - start) * 1000.0 / (double)mTimestampFreq);
        mSamples[i][mRingHead[i]] = ms;
        mRingHead[i] = (mRingHead[i]+1) % kRingSize;
    }
    rb->Unmap(0, nullptr);
}

float GpuTimer::GetAverageMs(TimerID id, int windowFrames) const {
    if (id >= mNames.size()) return 0.0f;
    const int w = std::min(std::max(windowFrames, 1), kRingSize);
    const auto& ring = mSamples[id];
    const int start = (mRingHead[id] + kRingSize - w) % kRingSize;
    float sum = 0.0f;
    for(int i =0; i < w; i++)
        sum += ring[(start + i) % kRingSize];
    return sum / static_cast<float>(w);
}

GpuTimer::Scoped::Scoped(ID3D12GraphicsCommandList* cmdList,
    GpuTimer& timer, TimerID id, const char* name)
    : mCmdList(cmdList), mTimer(timer), mId(id) {
    PIXBeginEvent(mCmdList, PIX_COLOR_DEFAULT, name);
    mTimer.BeginTimer(mCmdList, mId);
}

GpuTimer::Scoped::~Scoped() {
    mTimer.EndTimer(mCmdList, mId);
    PIXEndEvent(mCmdList);
}