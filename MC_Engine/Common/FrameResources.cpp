#include "FrameResource.h"

FrameResource::FrameResource(ID3D12Device* device, UINT passCount, UINT objCount, UINT materialCount, UINT depthCount, UINT instanceCount, UINT  grassInstanceCount) {
	ThrowIfFailed(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));
	PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true);
	MaterialCB = std::make_unique<UploadBuffer<MaterialConstants>>(device, materialCount, false); // d3dUtil.h  // will use as SRV (StructuredBuffer<MaterialData> gMaterialData : register(t0))
	ObjectCB = std::make_unique<UploadBuffer<PerObjectCB>>(device, objCount, true);
	DepthCB = std::make_unique<UploadBuffer<DebugDepthConstants>>(device, objCount, true);  // FrameResource.h
    InstanceCB = instanceCount > 0 ? std::make_unique < UploadBuffer <InstanceData>>(device, instanceCount, false) : nullptr;  // FrameResource.h 
    GrassInstanceCB = grassInstanceCount > 0 ? std::make_unique<UploadBuffer<GrassInstanceData>>(device, grassInstanceCount, false) : nullptr;
    SobelCB = std::make_unique<UploadBuffer<CSB_default>>(device, 1, 1);
    

    D3D12_QUERY_HEAP_DESC qhDesc = {};
    qhDesc.Count = GpuTimerCount;
    qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.NodeMask = 0;

    ThrowIfFailed(device->CreateQueryHeap(
        &qhDesc,
        IID_PPV_ARGS(GpuTimestampHeap.GetAddressOf())));

    UINT64 bufferSize = sizeof(UINT64) * GpuTimerCount;
    auto bufResDsc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
    auto heapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    ThrowIfFailed(device->CreateCommittedResource(
        &heapType,
        D3D12_HEAP_FLAG_NONE,
        &bufResDsc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(GpuTimestampReadback.GetAddressOf())));
}

FrameResource::~FrameResource() {}