#pragma once
#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include "Common/d3dx12.h" 

// #include "d3dUtil.h"
struct ID3D12Resource;

// using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct MCVec2 {
	float x;
	float y;
};

struct MCVec3 {
	float x;
	float y;
	float z;
};

typedef
enum MC_D3_RESOURCE_TYPE {
	MC_D3_RESOURCE_TYPE_TEXTURE = 0,
	MC_D3_RESOURCE_TYPE_BUFFER,
	MC_D3_RESOURCE_TYPE_COUNT
} MC_D3_RESOURCE_TYPE;

struct MC_DESC_HEAP_MANAGER_DESC {
	ID3D12Device* device;                   // used to create the CPU-only per-tier staging heaps
	ID3D12DescriptorHeap* csuCombinedHeap;  // shader-visible combined CBV/SRV/UAV heap (owned by MCEngine, passed in)
	ID3D12DescriptorHeap* rtvHeap;
	ID3D12DescriptorHeap* dsvHeap;
	UINT csuDescSize;
	UINT rtvDescSize;
	UINT dsvDescSize;
	int NumFrameResources;
	int rtvHeapMaxCap;
	int dsvHeapMaxCap;
	int rtvReservedHead = 0;		// slots reserved at the head of the RTV heap (e.g. SwapChainBufferCount for the back buffers)
	int dsvReservedHead = 0;		// slots reserved at the head of the DSV heap (e.g. 1 for the app-owned depth buffer)

	int csuTierStaticCap;			// - per-tier capacities within the combined heap; CPU-only tier heaps are created
	int csuTierDynamicCap;			//   internally by DescHeapManager with matching capacities
	int csuReservedHead;			// - slots reserved at the head of the combined heap before Static tier (e.g. 2 for ImGui)
	// invariant: csuCombinedHeap->GetDesc().NumDescriptors == csuReservedHead + csuTierStaticCap + csuTierDynamicCap
};

typedef
enum MC_VIEW_TIER {
	MC_VIEW_TIER_STATIC = 0,
	MC_VIEW_TIER_DYNAMIC,
	MC_VIEW_TIER_COUNT
} MC_VIEW_TIER;

struct DescHeapHandle {
	CD3DX12_CPU_DESCRIPTOR_HANDLE hCpu;
	CD3DX12_GPU_DESCRIPTOR_HANDLE hGpu;
	int offset = -1;
	MC_VIEW_TIER tier = MC_VIEW_TIER_STATIC;
};

class MCResource {
public:
	std::string Name;
	ComPtr<ID3D12Resource> mResource;
	ComPtr<ID3D12Resource> UploadHeap;
	virtual ~MCResource() = default;
	MC_D3_RESOURCE_TYPE Type = MC_D3_RESOURCE_TYPE::MC_D3_RESOURCE_TYPE_COUNT;
	D3D12_RESOURCE_STATES m_currState = D3D12_RESOURCE_STATE_COMMON;	// defaults to COMMON to allow implicit promotion
protected:
	MCResource(MC_D3_RESOURCE_TYPE type, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON) : Type(type), m_currState(state) {}
};

class MCTexture : public MCResource {
public:
	MCTexture() : MCResource(MC_D3_RESOURCE_TYPE_TEXTURE) {}
	MCTexture(D3D12_RESOURCE_STATES state) : MCResource(MC_D3_RESOURCE_TYPE_TEXTURE, state) {}
	std::wstring Filename;

	std::vector<DescHeapHandle> SRVs = {};
	std::vector<DescHeapHandle> UAVs = {};
	std::vector<DescHeapHandle> RTVs = {};
	std::vector<DescHeapHandle> DSVs = {};
};

class MCBuffer : public MCResource {
public:
	MCBuffer() : MCResource(MC_D3_RESOURCE_TYPE_BUFFER) {}
	MCBuffer(D3D12_RESOURCE_STATES state) : MCResource(MC_D3_RESOURCE_TYPE_BUFFER, state) {}
	UINT64 firstElement = 0;
	UINT elementCount = 0;
	UINT elementByteSize = 0;
	std::vector<DescHeapHandle> CBVs = {};
	std::vector<DescHeapHandle> SRVs = {};
	std::vector<DescHeapHandle> UAVs = {};
};