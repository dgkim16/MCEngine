#pragma once

#include "d3dUtil.h"
#include "GameTimer.h"

#define _DEBUG

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxcompiler.lib")

using namespace Microsoft::WRL; // ComPtr

class D3DApp
{
protected:
	D3DApp(HINSTANCE hInstance); // constructor
	D3DApp(const D3DApp& rhs) = delete; // remove copy constructor
	D3DApp& operator=(const D3DApp& rhs) = delete; // remove copy assignment
	// copy assignment: Used when overwriting an already existing object.
	virtual ~D3DApp(); // virtual destructor - virtual allows inherited classes to derive delete as well

public:
	static D3DApp* GetApp();
	HINSTANCE AppInst()const;
	HWND MainWnd()const;
	float AspectRatio()const;

	bool Get4xMsaaState()const;
	void Set4xMsaaState(bool value);

	int Run();
	virtual bool Initialize();
	virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// virtual functions
protected:
	virtual void CreateRtvAndDsvDescriptorHeaps();
	virtual void OnResize();
	virtual void Update(GameTimer& gt) = 0;
	virtual void Draw(const GameTimer& gt) = 0;

	// Convenience overrides for handling mouse input.
	virtual void OnMouseDown(WPARAM btnState, int x, int y) {}
	virtual void OnMouseUp(WPARAM btnState, int x, int y) {}
	virtual void OnMouseMove(WPARAM btnState, int x, int y) {}

// non virtual functions
protected:
	bool InitMainWindow();
	bool InitDirect3D();
	void CreateCommandObjects();
	void CreateSwapChain();
	void FlushCommandQueue();

	ID3D12Resource* CurrentBackBuffer()const;
	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView()const;
	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView()const;

	void CalculateFrameStats();

	void LogAdapters();
	void LogAdapterOutputs(IDXGIAdapter* adapter);
	void LogOutputDisplayModes(IDXGIOutput* output, DXGI_FORMAT format);
	void UpdateCurrentMonitorRefreshRate(HWND hwnd);

protected:
	static D3DApp* mApp; // singleton design

	HINSTANCE mhAppInst = nullptr; // application instance handle
	HWND      mhMainWnd = nullptr; // main window handle
	bool      mAppPaused = false;  // is the application paused?
	bool      mMinimized = false;  // is the application minimized?
	bool      mMaximized = false;  // is the application maximized?
	bool      mResizing = false;   // are the resize bars being dragged?
	bool      mFullscreenState = false;// fullscreen enabled

	// Set true to use 4X MSAA (?.1.8).  The default is false.
	bool	  m4xMsaaState = false;
	UINT	  m4xMsaaQuality = 0;

	

	GameTimer mTimer;

	// Declare variables
	ComPtr<IDXGIFactory4> mdxgiFactory; // dxgi1_4.h
	ComPtr<IDXGISwapChain> mSwapChain;
	ComPtr<ID3D12Device> md3dDevice; // d3d12.h

	ComPtr<ID3D12Fence> mFence;
	UINT64 mCurrentFence = 0;

	ComPtr<ID3D12CommandQueue> mCommandQueue;
	ComPtr<ID3D12CommandAllocator> mDirectCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList> mCommandList;

	static const int SwapChainBufferCount = 2;
	int mCurrBackBuffer = 0;
	ComPtr<ID3D12Resource> mSwapChainBuffer[SwapChainBufferCount];
	ComPtr<ID3D12Resource> mDepthStencilBuffer;

	ComPtr<ID3D12DescriptorHeap> mRtvHeap;
	ComPtr<ID3D12DescriptorHeap> mDsvHeap;

	D3D12_VIEWPORT mScreenViewport;		// value assigned in OnResize()
	D3D12_RECT mScissorRect;			// value assigned in OnResize()

	UINT mRtvDescriptorSize = 0;			// value assigned in D3DApp::create_Fence_Query_Descriptor_size()
	UINT mDsvDescriptorSize = 0;			// value assigned in D3DApp::create_Fence_Query_Descriptor_size()
	UINT mCbvSrvUavDescriptorSize = 0;		// value assigned in D3DApp::create_Fence_Query_Descriptor_size()

	// Derived class should set these in derived constructor to customize starting values.
	std::wstring mMainWndCaption = L"d3d App";
	D3D_DRIVER_TYPE md3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
	DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	
	int mClientWidth = 1280;
	int mClientHeight = 800;
	bool capFPS = false;
	double targetFPS = 60.0; // modified by GameTimer
	int maxFPS = 60;
	bool mTearingSupported = false;	// for tearing (disables vsync by dxgi)
	float nearPlane = 0.01f;
	float farPlane = 1000.0f;

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

