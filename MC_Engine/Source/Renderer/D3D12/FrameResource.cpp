#include "Renderer/D3D12/FrameResource.h"
#include "Renderer/D3D12/ConstExpressionValues.h"

FrameResource::FrameResource(ID3D12Device* device, UINT passCount, UINT objCount, UINT materialCount, UINT depthCount, UINT instanceCount, UINT  grassInstanceCount) {
	ThrowIfFailed(device->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));
	PassCB = std::make_unique<UploadBuffer<PerPassCB>>(device, passCount, true); // only one affected by passCount
	MaterialCB = std::make_unique<UploadBuffer<MaterialConstants>>(device, materialCount, false); // d3dUtil.h  // will use as SRV (StructuredBuffer<MaterialData> gMaterialData : register(t0))
	// ObjectCB = std::make_unique<UploadBuffer<PerObjectCB>>(device, objCount, true);
	ObjectCB = std::make_unique<UploadBuffer<InstanceData>>(device, objCount, false);
	DepthCB = std::make_unique<UploadBuffer<DebugDepthConstants>>(device, objCount, true);  // FrameResource.h
	LightCB = std::make_unique<UploadBuffer<LightData>>(device, /*capacity*/ kLightBufferCapacity, false);
    InstanceCB = instanceCount > 0 ? std::make_unique < UploadBuffer <InstanceData>>(device, instanceCount, false) : nullptr;  // FrameResource.h 
    GrassInstanceCB = grassInstanceCount > 0 ? std::make_unique<UploadBuffer<GrassInstanceData>>(device, grassInstanceCount, false) : nullptr;
}

FrameResource::~FrameResource() {}