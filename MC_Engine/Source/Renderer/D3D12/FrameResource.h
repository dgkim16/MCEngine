#pragma once
#include "SharedTypes.h"
#include "Renderer/D3D12/d3dUtil.h"
#include "Renderer/D3D12/UploadHelper.h"
#include "Core/MathHelper.h"
#include <array>

// circular array of frame Resource
// typically an array of 3 frame resources
struct FrameResource {
public:
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount, UINT depthCount, UINT instanceCount = 0, UINT grassInstanceCount = 0);
    ~FrameResource();
    FrameResource(const FrameResource& rhs) = delete; // copy
    FrameResource& operator=(const FrameResource& rhs) = delete;
    FrameResource(FrameResource&& rhs) = default; // move
    FrameResource& operator=(FrameResource&& rhs) = default;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc; // each frame gets its own allocator

    // each frame has their own constant buffers (mUploadBuffer)
    std::unique_ptr<UploadBuffer<PerPassCB>> PassCB = nullptr;
    // std::unique_ptr<UploadBuffer<PerObjectCB>> ObjectCB = nullptr; // before migration to bindlless perObjectCB
    std::unique_ptr<UploadBuffer<InstanceData>> ObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
    std::unique_ptr<UploadBuffer<DebugDepthConstants>> DepthCB = nullptr;
    std::unique_ptr<UploadBuffer<LightData>> LightCB = nullptr;

    std::unique_ptr<UploadBuffer<InstanceData>> InstanceCB = nullptr;
    std::unique_ptr<UploadBuffer<GrassInstanceData>> GrassInstanceCB = nullptr;

    PerPassCB mCachedMainPassCB;

    // for checking if this frame is still in use by GPU
    UINT64 Fence = 0;
};