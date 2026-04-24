#pragma once
#include "../Assets/Shaders/SharedTypes.h"
#include "d3dUtil.h"
#include "UploadHelper.h"
#include "MathHelper.h"
#include <array>

using namespace Microsoft::WRL;

// circular array of frame Resource
// typically an array of 3 frame resources
struct FrameResource {
public:
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT depthCount, UINT instanceCount = 0, UINT grassInstanceCount = 0);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();

    ComPtr<ID3D12CommandAllocator> CmdListAlloc; // each frame gets its own allocator

    // each frame has their own constant buffers (mUploadBuffer)
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<PerObjectCB>> ObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
    std::unique_ptr<UploadBuffer<DebugDepthConstants>> DepthCB = nullptr;

    std::unique_ptr<UploadBuffer<InstanceData>> InstanceCB = nullptr;
    std::unique_ptr<UploadBuffer<GrassInstanceData>> GrassInstanceCB = nullptr;

    // for checking if this frame is still in use by GPU
    UINT64 Fence = 0;

    // GpuTimerCount : time measuring points
    static const UINT GpuTimerCount = 10; // @begin, after: scene color, depth debug, msaa resolve, force alpha, blurs, sobel, change render target / depth to back buffer, imgui, present
    ComPtr<ID3D12QueryHeap> GpuTimestampHeap;
    ComPtr<ID3D12Resource>  GpuTimestampReadback;
    std::array<double, GpuTimerCount-1> GpuFrameMs = {};
    double totalGpuFrameMs = 0;
};