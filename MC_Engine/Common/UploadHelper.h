#pragma once
#include "d3dUtil.h"

//! Wrapper for constant buffer that updates frequently
// T is used for identifying the size of data
template<typename T>
class UploadBuffer
{
private:
	Microsoft::WRL::ComPtr<ID3D12Resource> mUploadBuffer;
	BYTE* mMappedData = nullptr;			// where the CPU writes to (in init stage, mapped to GPU resource mUploadBuffer)
	UINT mElementByteSize = 0;
	bool mIsConstantBuffer = false;

public:
	UploadBuffer(ID3D12Device* device, UINT elementCount, bool isConstantBuffer) :
		mIsConstantBuffer(isConstantBuffer) 
	{
		mElementByteSize = sizeof(T);
		// pad to mult of 256 Bytes
		if (isConstantBuffer)
			mElementByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(T));

		auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufferSize = CD3DX12_RESOURCE_DESC::Buffer(mElementByteSize * elementCount);
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&bufferSize,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mUploadBuffer)
		));

		// Map() writes into the pointer (ppData) passed into it (Gets a CPU pointer to the specified subresource in the resource)
		// DirectX write the mapped CPU memory address into that pointer (CPU-visible upload heap memory)
		// Map() requires void**, but your variable is BYTE*. So cast BYTE** to void** .
		ThrowIfFailed(mUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData)));
	}
	// ex:
	// std::unique_ptr<UploadBuffer<ObjectConstants>> mObjectCB = nullptr;
	// ...
	// mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(md3dDevice.Get(), 1, true);

	// Prevent Copying of this class
	UploadBuffer(const UploadBuffer& rhs) = delete; // delete copy constructor
	UploadBuffer& operator=(const UploadBuffer& rhs) = delete; // delete copy assignment operator
	~UploadBuffer() {
		if (mUploadBuffer != nullptr)
			mUploadBuffer->Unmap(0, nullptr);
		mMappedData = nullptr;
	}

	ID3D12Resource* Resource()const {
		return mUploadBuffer.Get();		// returns ID3D12Resource
	}

	void Reset() {
		mUploadBuffer.Reset();
	}

	void CopyData(int elementIndex, const T& data) {
		memcpy(&mMappedData[elementIndex * mElementByteSize], &data, sizeof(T)); 
		// copy input data into mMappedData at index elementIndex
		// this automatically updates mUploadBuffer
	}
};