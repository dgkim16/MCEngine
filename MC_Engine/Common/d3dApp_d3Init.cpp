#include "d3dApp.h"
#include <WindowsX.h>

ID3D12Resource* D3DApp::CurrentBackBuffer()const
{
	return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView()const
{
	// returns wrapper (inherited class) for D3D12_CPU_DESCRIPTOR_HANDLE
	// which is a CPU handle
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView()const
{
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void D3DApp::create_Device() {
	// Create ID3D12Device
	ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));
	HRESULT hwResult = D3D12CreateDevice(
		nullptr,
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&md3dDevice)
	);

	// if failed, fallback to WARP (Windows Advanced Rasterization Platform
	if (FAILED(hwResult)) {
		ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));
		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&md3dDevice)
		));
	}

	ComPtr<IDXGIFactory5> factory5;
	if (SUCCEEDED(mdxgiFactory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(
			DXGI_FEATURE_PRESENT_ALLOW_TEARING,
			&allowTearing,
			sizeof(allowTearing))))
		{
			mTearingSupported = (allowTearing == TRUE);
		}
	}
	std::cout << "Tearing supported: " << mTearingSupported << std::endl;
}

void D3DApp::create_Fence_Query_Descriptor_size() {
	ThrowIfFailed(md3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
	mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// returns true if enabled by default
void D3DApp::check_MSAAx4() {
	// check supported quality model
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = mBackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(md3dDevice->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)));

	m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	std::cout << "NumQualityLevels : " << m4xMsaaQuality << std::endl;
	assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");
}

void D3DApp::create_cmdQueue() {
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)));
}

void D3DApp::create_cmdListAllocator() {
	ThrowIfFailed(md3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(mDirectCmdListAlloc.GetAddressOf())));
}

void D3DApp::create_cmdList() {
	ThrowIfFailed(md3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc.Get(), // Associated command allocator
		nullptr,                   // Initial PipelineStateObject
		IID_PPV_ARGS(mCommandList.GetAddressOf())));
	mCommandList->Close();
}

void D3DApp::CreateCommandObjects()
{
	create_cmdQueue();
	create_cmdListAllocator();
	create_cmdList();
}

void D3DApp::CreateSwapChain() {
	// Release the previous swapchain we will be recreating.
	mSwapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd = {};
	// everytime this function is called, sd is recreated dynamically using these non-const variables.
	// allows us to destroy and create new swapchain in runtime.
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = mhMainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (mTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

	// std::cout << "target sampleDesc.quality : " << sd.SampleDesc.Quality << std::endl;
	// monitors by default can't display msaa texture
	// DX12 no longer internally resolves msaa data to non-msaa texture
	// so the book is outdated

	// Note: Swap chain uses queue to perform flush.
	ThrowIfFailed(mdxgiFactory->CreateSwapChain(
		mCommandQueue.Get(),
		&sd,
		mSwapChain.GetAddressOf()));

	// swap chain's buffer is not created yet
	// thus the buffer resource is not 'viewed' to 'Resource' either
	// OnResize() does the creation and viewing 
}

void D3DApp::CreateRtvAndDsvDescriptorHeaps() {
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc,
		IID_PPV_ARGS(mRtvHeap.GetAddressOf())
	));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc,
		IID_PPV_ARGS(mDsvHeap.GetAddressOf())
	));
}

// forces CPU to wait until GPU finished processing all commands in the queue upto specified fence point
void D3DApp::FlushCommandQueue() {
	mCurrentFence++;
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));
	if (mFence->GetCompletedValue() < mCurrentFence) {
		HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

// called by D3DAPP::MsgProc when WM_SIZE msg is received
void D3DApp::OnResize() {
	// assert if these are nullptr
	assert(md3dDevice);
	assert(mSwapChain);
	assert(mDirectCmdListAlloc);

	// flush and reset cmdqueue and cmdlist
	FlushCommandQueue();
	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	// reset swapchain buffers and DS_buffer
	for (int i = 0; i < SwapChainBufferCount; ++i)
		mSwapChainBuffer[i].Reset();
	mDepthStencilBuffer.Reset();
	UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
		| (mTearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	// Resize SwapChain. Dont recreate them.
	ThrowIfFailed(mSwapChain->ResizeBuffers(
		SwapChainBufferCount, mClientWidth, mClientHeight,
		mBackBufferFormat,
		swapChainFlags));
	mCurrBackBuffer = 0;

	// ch 4.3.7 -- create RTV
	// 1. setup resource as the ith back buffer in swapchain
	// 2. create ResourceView(RTV) to that ith resource
	// Result: Binding of ResourceView to pipeline stage (back buffer binds to output merger stage of pipeline)
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < SwapChainBufferCount; i++) {
		// get ith buffer in swap chain and store in member variable (did .Reset() to it before)
		ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
		// create rtv
		md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
		mSwapChainBuffer[i].Get()->SetName((L"SwapChainBuffer (RenderTarget) - " + std::to_wstring(i)).c_str());
		// next entry in heap
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}

	// ch 4.3.8 -- create Depth/Stencil Buffer View (which is just a 2D texture)
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = mClientWidth;
	depthStencilDesc.Height = mClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1; // only need 1 for D/SBV

	depthStencilDesc.Format = mDepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	depthStencilDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;


	D3D12_CLEAR_VALUE optClear;
	optClear.Format = mDepthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;
	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
	// D3D12_HEAP_TYPE_DEFAULT : resources here are solely accessed by GPU (CPU never access) 

	// D/S Buffer will be a GPU resource
	// create and commit a resource to GPU heap like this
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())));

	// create View to DSBV
	md3dDevice->CreateDepthStencilView(
		mDepthStencilBuffer.Get(),
		nullptr,
		DepthStencilView()
	);

	// Transition resource state (purpose: to be used as depth buffer)
	auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
		mDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_DEPTH_WRITE
	);
	mCommandList->ResourceBarrier(1, &transition); // TODO(barrier-tracker): D3DApp backbone's Resource are not wrapped by MCTexture. When splitting into runtime / editor, may be (phase 3)
	OutputDebugString(L"OnResize - Tyring to close cmdlist...\n");
	ThrowIfFailed(mCommandList->Close());
	OutputDebugString(L"OnResize - SUCCESS to close cmdlist...\n");
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);
	FlushCommandQueue(); // wait (stall process)

	// Update the viewport transform to cover the client area.
	mScreenViewport.TopLeftX = 0;
	mScreenViewport.TopLeftY = 0;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MinDepth = 0.0f;
	mScreenViewport.MaxDepth = 1.0f;

	mScissorRect = { 0, 0, mClientWidth, mClientHeight };
}

bool D3DApp::InitDirect3D() {
#if defined(DEBUG) || defined(_DEBUG) 
	// detect PIX at launch
	bool runningUnderPix =
		GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr ||
		GetModuleHandleW(L"WinPixTimingCapturer.dll") != nullptr;

	// ============== DEBUG LAYER =================
	// Enable the D3D12 debug layer.
	{
		ComPtr<ID3D12Debug> debugController;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)));
		debugController->EnableDebugLayer();
		if (!runningUnderPix) {
			ComPtr<ID3D12Debug1> debugController1;
			if (SUCCEEDED(debugController.As(&debugController1))) {
				// enable GPU-Based Validation (GBV) before creating a device with the debug layer enabled.
				// overhead is non-trivial, but not signficant. Make sure to disable this when using PIX to profile performance.
				debugController1->SetEnableGPUBasedValidation(TRUE);
			}
		}
	}
#endif
	create_Device();
#if defined(DEBUG) || defined(_DEBUG) 
	Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(md3dDevice.As(&infoQueue)))
	{
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		if(!runningUnderPix)
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		D3D12_MESSAGE_ID denyIds[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
		};

		D3D12_INFO_QUEUE_FILTER filter = {};
		filter.DenyList.NumIDs = _countof(denyIds);
		filter.DenyList.pIDList = denyIds;
		infoQueue->AddStorageFilterEntries(&filter);
	}
#endif
	create_Fence_Query_Descriptor_size();
	check_MSAAx4();
#ifdef _DEBUG
	LogAdapters();
#endif
	CreateCommandObjects();
	std::cout << "Created CMD Objects" << std::endl;
	CreateSwapChain();
	std::cout << "Created SwapChain" << std::endl;
	CreateRtvAndDsvDescriptorHeaps();
	std::cout << "Created RTV DSV desc heaps" << std::endl;
return true;
}
