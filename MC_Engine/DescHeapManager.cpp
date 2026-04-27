
#include "DescHeapManager.h"

DescHeapManager::DescHeapManager(MC_DESC_HEAP_MANAGER_DESC desc) :
	mRtvHeap(desc.rtvHeap),
	mDsvHeap(desc.dsvHeap),
	NumFrameResources(desc.NumFrameResources),
	mRtv(desc.rtvDescSize, desc.rtvHeapMaxCap),
	mDsv(desc.dsvDescSize, desc.dsvHeapMaxCap),
	device(desc.device)
{
	assert(desc.device && "MC_DESC_HEAP_MANAGER_DESC::device is required");
	assert(desc.csuCombinedHeap && "MC_DESC_HEAP_MANAGER_DESC::csuCombinedHeap is required");

	// Skip past slots reserved by D3DApp (swap-chain back buffers in the RTV heap,
	// the app-owned depth buffer in the DSV heap) so allocations start after them.
	mRtv.lastOffset = desc.rtvReservedHead;
	mDsv.lastOffset = desc.dsvReservedHead;
	mRtv.reservedHead = desc.rtvReservedHead;
	mDsv.reservedHead = desc.dsvReservedHead;

	mCSU.combined     = desc.csuCombinedHeap;
	mCSU.descSize     = desc.csuDescSize;
	mCSU.reservedHead = desc.csuReservedHead;

	const int tierCaps[MC_VIEW_TIER_COUNT] = {
		desc.csuTierStaticCap,
		desc.csuTierDynamicCap,
	};

	// Combined heap must be sized exactly = reservedHead + sum(tierCaps). Otherwise bindless indices
	// computed from baseOffset[tier] + localOff will walk off the end.
	const UINT combinedNumDescs = mCSU.combined->GetDesc().NumDescriptors;
	const int  expectedNumDescs = desc.csuReservedHead + tierCaps[0] + tierCaps[1];
	assert((int)combinedNumDescs == expectedNumDescs
		&& "csuCombinedHeap NumDescriptors must equal csuReservedHead + sum of tier capacities");

	int runningBase = desc.csuReservedHead;
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t) {
		mCSU.baseOffset[t] = runningBase;
		runningBase += tierCaps[t];

		mCSU.tiers[t] = DHInfo(desc.csuDescSize, tierCaps[t]);
		mCSU.dirty[t] = false;

		if (tierCaps[t] <= 0) continue;

		D3D12_DESCRIPTOR_HEAP_DESC cpuHeapDesc = {};
		cpuHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		cpuHeapDesc.NumDescriptors = (UINT)tierCaps[t];
		cpuHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		cpuHeapDesc.NodeMask       = 0;
		ThrowIfFailed(desc.device->CreateDescriptorHeap(&cpuHeapDesc, IID_PPV_ARGS(mCSU.cpuHeap[t].GetAddressOf())));
	}
}

void DescHeapManager::Update() {
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t)
		UpdatePending(mCSU.tiers[t]);
	UpdatePending(mRtv);
	UpdatePending(mDsv);
}

void DescHeapManager::UpdatePending(DHInfo& dhi) {
	for (size_t i = 0; i < dhi.fpl.size();) {
		dhi.fpl[i].counter--;
		if (dhi.fpl[i].counter <= 0) {
			RemoveFromSet(dhi.fpl[i].idx, dhi);
			dhi.fpl[i] = dhi.fpl.back(); // overwrite current entry with last entry
			dhi.fpl.pop_back();          // remove last entry
		}
		else
			i++;
	}
}

void DescHeapManager::FlushPending() {
	auto drain = [this](DHInfo& dhi) {
		for (auto& e : dhi.fpl) RemoveFromSet(e.idx, dhi);
		dhi.fpl.clear();
	};
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t)
		drain(mCSU.tiers[t]);
	drain(mRtv);
	drain(mDsv);
}

void DescHeapManager::Shutdown() {
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t)
		mCSU.cpuHeap[t].Reset();
	mCSU.combined = nullptr;
	mRtvHeap = nullptr;
	mDsvHeap = nullptr;
	device = nullptr;
}

void DescHeapManager::CommitToShaderVisible() {
	static int sFrameCount = 0;
	for (int t = 0; t < MC_VIEW_TIER_COUNT; ++t) {
		sFrameCount++;
		if (!mCSU.dirty[t]) continue;
		std::cout << "Frame ["<<sFrameCount<<"] [DescHeapManager.cpp] Tier: " << t << " was dirty.Now Copying..." << std::endl;
		auto& T = mCSU.tiers[t];
		if (T.lastOffset <= 0) { mCSU.dirty[t] = false; continue; }

		// First-pass strategy: copy the whole used range of the tier. Range-based
		// dirty tracking is a follow-up once we measure per-frame copy cost.
		CPUH dst(mCSU.combined->GetCPUDescriptorHandleForHeapStart(), mCSU.baseOffset[t], mCSU.descSize);
		CPUH src(mCSU.cpuHeap[t]->GetCPUDescriptorHandleForHeapStart(), 0,                mCSU.descSize);
		device->CopyDescriptorsSimple((UINT)T.lastOffset, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		mCSU.dirty[t] = false;
	}
}

void DescHeapManager::CreateCbv(MCBuffer& /*buffer*/, MC_VIEW_TIER /*tier*/) {
	// CBVs are bound directly via SetRootConstantBufferView — no descriptor heap slot needed.
}

void DescHeapManager::CreateSrv2d(MCTexture& mcResource, DXGI_FORMAT format, bool isMSAA, MC_VIEW_TIER tier) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = isMSAA ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip     = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Format                        = format;
	srvDesc.Texture2D.MipLevels           = resource->GetDesc().MipLevels;

	const int tierIdx = (int)tier;
	auto& T = mCSU.tiers[tierIdx];
	const int localOff  = T.allocate();
	const int globalOff = mCSU.baseOffset[tierIdx] + localOff;

	auto hStage = hCSU_Stage(tierIdx, localOff);
	device->CreateShaderResourceView(resource, &srvDesc, hStage);

	DescHeapHandle dhH;
	dhH.hCpu   = hStage;
	dhH.hGpu   = hCSU_CombinedGpu(globalOff);
	dhH.offset = globalOff;
	dhH.tier   = tier;

	mcResource.SRVs.push_back(dhH);
	T.Offset_Set.insert(localOff);
	T.slotInfo[localOff] = { mcResource.Name, "SRV2D", format };
	mCSU.dirty[tierIdx] = true;
}

void DescHeapManager::CreateSrvCube(MCTexture& mcResource, DXGI_FORMAT format, bool isMSAA, MC_VIEW_TIER tier) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = isMSAA ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip     = 0;
	srvDesc.Texture2DArray.FirstArraySlice     = 0;
	srvDesc.Texture2DArray.ArraySize           = resource->GetDesc().DepthOrArraySize;
	srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
	srvDesc.Format                             = format;
	srvDesc.Texture2DArray.MipLevels           = resource->GetDesc().MipLevels;

	const int tierIdx = (int)tier;
	auto& T = mCSU.tiers[tierIdx];
	const int localOff  = T.allocate();
	const int globalOff = mCSU.baseOffset[tierIdx] + localOff;

	auto hStage = hCSU_Stage(tierIdx, localOff);
	device->CreateShaderResourceView(resource, &srvDesc, hStage);

	DescHeapHandle dhH;
	dhH.hCpu   = hStage;
	dhH.hGpu   = hCSU_CombinedGpu(globalOff);
	dhH.offset = globalOff;
	dhH.tier   = tier;

	mcResource.SRVs.push_back(dhH);
	T.Offset_Set.insert(localOff);
	T.slotInfo[localOff] = { mcResource.Name, "SRVCube", format };
	mCSU.dirty[tierIdx] = true;
}

void DescHeapManager::CreateSrvBuffer(MCBuffer& mcResource, MC_VIEW_TIER tier) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format                       = DXGI_FORMAT_UNKNOWN; // structured buffer
	srvDesc.ViewDimension                = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc.Shader4ComponentMapping      = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Buffer.FirstElement          = mcResource.firstElement;
	srvDesc.Buffer.NumElements           = mcResource.elementCount;
	srvDesc.Buffer.StructureByteStride   = mcResource.elementByteSize;
	srvDesc.Buffer.Flags                 = D3D12_BUFFER_SRV_FLAG_NONE;

	const int tierIdx = (int)tier;
	auto& T = mCSU.tiers[tierIdx];
	const int localOff  = T.allocate();
	const int globalOff = mCSU.baseOffset[tierIdx] + localOff;

	auto hStage = hCSU_Stage(tierIdx, localOff);
	device->CreateShaderResourceView(resource, &srvDesc, hStage);

	DescHeapHandle dhH;
	dhH.hCpu   = hStage;
	dhH.hGpu   = hCSU_CombinedGpu(globalOff);
	dhH.offset = globalOff;
	dhH.tier   = tier;

	mcResource.SRVs.push_back(dhH);
	T.Offset_Set.insert(localOff);
	T.slotInfo[localOff] = { mcResource.Name, "SRVBuf", DXGI_FORMAT_UNKNOWN };
	mCSU.dirty[tierIdx] = true;
}

void DescHeapManager::CreateUav2d(MCTexture& mcResource, DXGI_FORMAT format, UINT mipSlice, MC_VIEW_TIER tier) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format               = format;
	uavDesc.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice   = mipSlice;

	const int tierIdx = (int)tier;
	auto& T = mCSU.tiers[tierIdx];
	const int localOff  = T.allocate();
	const int globalOff = mCSU.baseOffset[tierIdx] + localOff;

	auto hStage = hCSU_Stage(tierIdx, localOff);
	device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, hStage);

	DescHeapHandle dhH;
	dhH.hCpu   = hStage;
	dhH.hGpu   = hCSU_CombinedGpu(globalOff);
	dhH.offset = globalOff;
	dhH.tier   = tier;

	mcResource.UAVs.push_back(dhH);
	T.Offset_Set.insert(localOff);
	T.slotInfo[localOff] = { mcResource.Name, "UAV2D", format };
	mCSU.dirty[tierIdx] = true;
}

void DescHeapManager::CreateUavBuffer(MCBuffer& mcResource, ID3D12Resource* counterResource, UINT64 counterOffset, MC_VIEW_TIER tier) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format                       = DXGI_FORMAT_UNKNOWN; // structured buffer
	uavDesc.ViewDimension                = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement          = mcResource.firstElement;
	uavDesc.Buffer.NumElements           = mcResource.elementCount;
	uavDesc.Buffer.StructureByteStride   = mcResource.elementByteSize;
	uavDesc.Buffer.CounterOffsetInBytes  = counterOffset;
	uavDesc.Buffer.Flags                 = D3D12_BUFFER_UAV_FLAG_NONE;

	const int tierIdx = (int)tier;
	auto& T = mCSU.tiers[tierIdx];
	const int localOff  = T.allocate();
	const int globalOff = mCSU.baseOffset[tierIdx] + localOff;

	auto hStage = hCSU_Stage(tierIdx, localOff);
	device->CreateUnorderedAccessView(resource, counterResource, &uavDesc, hStage);

	DescHeapHandle dhH;
	dhH.hCpu   = hStage;
	dhH.hGpu   = hCSU_CombinedGpu(globalOff);
	dhH.offset = globalOff;
	dhH.tier   = tier;

	mcResource.UAVs.push_back(dhH);
	T.Offset_Set.insert(localOff);
	T.slotInfo[localOff] = { mcResource.Name, "UAVBuf", DXGI_FORMAT_UNKNOWN };
	mCSU.dirty[tierIdx] = true;
}


void DescHeapManager::CreateRtv2d(MCTexture& mcResource, DXGI_FORMAT format, bool isMSAA, UINT mipSlice) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.ViewDimension         = isMSAA ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Format                = format;
	rtvDesc.Texture2D.MipSlice    = mipSlice;
	rtvDesc.Texture2D.PlaneSlice  = 0;

	DescHeapHandle dhH;
	int offset = mRtv.allocate();
	auto hDesc = hDescRtv(offset);
	device->CreateRenderTargetView(resource, &rtvDesc, hDesc);
	dhH.hCpu   = hDesc;
	dhH.offset = offset;
	mcResource.RTVs.push_back(dhH);
	mRtv.Offset_Set.insert(offset);
	mRtv.slotInfo[offset] = { mcResource.Name, "RTV", format };
}

void DescHeapManager::CreateDsv(MCTexture& mcResource, D3D12_DSV_FLAGS flags, DXGI_FORMAT format, bool isMSAA, UINT mipSlice) {
	ID3D12Resource* resource = mcResource.mResource.Get();
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags               = flags;
	dsvDesc.ViewDimension       = isMSAA ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format              = format;
	dsvDesc.Texture2D.MipSlice  = mipSlice;

	DescHeapHandle dhH;
	int offset = mDsv.allocate();
	auto hDesc = hDescDsv(offset);
	device->CreateDepthStencilView(resource, &dsvDesc, hDesc);
	dhH.hCpu   = hDesc;
	dhH.offset = offset;
	mcResource.DSVs.push_back(dhH);
	mDsv.Offset_Set.insert(offset);
	mDsv.slotInfo[offset] = { mcResource.Name, "DSV", format };
}

void DescHeapManager::QueueRemovalFromSet(int idx, DHInfo& map) {
	if (map.Offset_Set.find(idx) == map.Offset_Set.end()) { // not allocated
		assert(false && "Trying to queue removal on non-allocated");
		return;
	}
	// check if it is in pending list. If it is, return
	for (auto& pEntry : map.fpl) {
		if (pEntry.idx == idx)
			return;
	}
	// Add to Pending
	map.fpl.push_back({ idx, NumFrameResources });
}

void DescHeapManager::QueueRemovalFromSet_CbvSrvUav(DescHeapHandle h) {
	const int tierIdx  = (int)h.tier;
	const int localOff = h.offset - mCSU.baseOffset[tierIdx];
	QueueRemovalFromSet(localOff, mCSU.tiers[tierIdx]);
}

// called from UpdatePending when the frame counter expires
void DescHeapManager::RemoveFromSet(int idx, DHInfo& map)
{
	auto it = map.Offset_Set.find(idx);
	if (it == map.Offset_Set.end()) { // not allocated
		assert(false && "Trying to remove on non-allocated");
		return;
	}
	map.Offset_Set.erase(it);
	map.freeList.push_back(idx);
	map.slotInfo[idx] = {};
}

void DescHeapManager::QueueRemoval_Texture(MCTexture& mcResource) {
	for (auto& dh : mcResource.SRVs)
		QueueRemovalFromSet_CbvSrvUav(dh);
	for (auto& dh : mcResource.UAVs)
		QueueRemovalFromSet_CbvSrvUav(dh);
	for (auto& dh : mcResource.RTVs)
		QueueRemovalFromSet_Rtv(dh);
	for (auto& dh : mcResource.DSVs)
		QueueRemovalFromSet_Dsv(dh);
}

void DescHeapManager::QueueRemoval_Buffer(MCBuffer& mcResource) {
	for (auto& dh : mcResource.CBVs)
		QueueRemovalFromSet_CbvSrvUav(dh);
	for (auto& dh : mcResource.SRVs)
		QueueRemovalFromSet_CbvSrvUav(dh);
	for (auto& dh : mcResource.UAVs)
		QueueRemovalFromSet_CbvSrvUav(dh);
}
