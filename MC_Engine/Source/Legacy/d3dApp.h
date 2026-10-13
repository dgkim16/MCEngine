#pragma once

#include "Renderer/D3D12/d3dUtil.h"
#include <dxgi1_4.h>

class D3DApp
{
protected:
	D3DApp() = default; // the window comes from PlatformSystem, through Initialize
	D3DApp(const D3DApp& rhs) = delete; // remove copy constructor
	D3DApp& operator=(const D3DApp& rhs) = delete; // remove copy assignment
	// copy assignment: Used when overwriting an already existing object.
	virtual ~D3DApp(); // virtual destructor - virtual allows inherited classes to derive delete as well

public:
	HWND MainWnd()const;
	float AspectRatio()const;

	bool Get4xMsaaState()const;
	void Set4xMsaaState(bool value);
	bool GetGbvState() const { return mGBVenabled; }

	bool Initialize(HWND window, int clientWidth, int clientHeight);
	

// virtual functions
protected:
	virtual void OnResize();
	virtual void RunFrame(float dt) = 0;	// one frame: update and draw

// non virtual functions
protected:
	bool InitDirect3D();
	void CreateCommandObjects();
	void CreateSwapChain();
	void FlushCommandQueue();

	ID3D12Resource* CurrentBackBuffer()const;
	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView()const;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView()const;

	void LogAdapters();
	void LogAdapterOutputs(IDXGIAdapter* adapter);
	void LogOutputDisplayModes(IDXGIOutput* output, DXGI_FORMAT format);

protected:
	HWND      mhMainWnd = nullptr; // main window handle
	bool      mFullscreenState = false;// fullscreen enabled

	// Set true to use 4X MSAA (?.1.8).  The default is false.
	bool	  m4xMsaaState = false;
	UINT	  m4xMsaaQuality = 0;

	// Declare variables
	Microsoft::WRL::ComPtr<IDXGIFactory4> mdxgiFactory; // dxgi1_4.h
	Microsoft::WRL::ComPtr<IDXGISwapChain3> mSwapChain;
	Microsoft::WRL::ComPtr<ID3D12Device> md3dDevice; // d3d12.h

	Microsoft::WRL::ComPtr<ID3D12Fence> mFence;
	UINT64 mCurrentFence = 0;

	Microsoft::WRL::ComPtr<ID3D12CommandQueue> mCommandQueue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;

	static const int SwapChainBufferCount = 2;
	int mCurrBackBuffer = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
	Microsoft::WRL::ComPtr<ID3D12Resource> mDepthStencilBuffer;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;

	D3D12_VIEWPORT mScreenViewport;		// value assigned in OnResize()
	D3D12_RECT mScissorRect;			// value assigned in OnResize()

	UINT mRtvDescriptorSize = 0;			// value assigned in D3DApp::create_Fence_Query_Descriptor_size()
	UINT mDsvDescriptorSize = 0;			// value assigned in D3DApp::create_Fence_Query_Descriptor_size()
	UINT mCbvSrvUavDescriptorSize = 0;		// value assigned in D3DApp::create_Fence_Query_Descriptor_size()

	// Derived class should set these in derived constructor to customize starting values.
	D3D_DRIVER_TYPE md3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
	DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; // Depth 24 bits unsigned normalized, stencil 8 bits uint
	DXGI_FORMAT mDepthShadowMapFormat = DXGI_FORMAT_R32_TYPELESS; // red-channel only, 32bits, typeless - interpret later (not fixed) -> DXGI_FORMAT_D32_FLOAT for DSV, DXGI_FORMAT_R32_FLOAT for SRV
	DXGI_FORMAT mDepthShadowMapDSVFormat = DXGI_FORMAT_D32_FLOAT;
	DXGI_FORMAT mDepthShadowMapSRVFormat = DXGI_FORMAT_R32_FLOAT;
	
	int mClientWidth = 1280;
	int mClientHeight = 800;

	bool mTearingSupported = false;	// for tearing (disables vsync by dxgi)
	float nearPlane = 0.01f;
	float farPlane = 1000.0f;
	bool mGBVenabled = false;

// Factored my way
private:
	void create_Device();
	void create_Fence_Query_Descriptor_size();
	void check_MSAAx4();
	void create_cmdQueue();
	void create_cmdListAllocator();
	void create_cmdList();
	// void describe_create_swapChain(); replaced with CreateSwapChain()
	// void CreateRtvAndDsvDescriptorHeaps();
	// void create_RenderTargetView(); replaced with OnResize() -- every resize = new RTVs
};

