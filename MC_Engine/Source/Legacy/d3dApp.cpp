#include "Legacy/d3dApp.h"
#include <winuser.h>
#include "App/Resource.h"

LRESULT CALLBACK
MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	// The window outlives the app when WinMain catches a fatal error: the error
	// dialog's message loop still dispatches here after the app is destroyed.
	if (D3DApp* app = D3DApp::GetApp())
		return app->MsgProc(hwnd, msg, wParam, lParam);
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Singleton design (I think)
D3DApp* D3DApp::mApp = nullptr;
D3DApp* D3DApp::GetApp() {
	return mApp;
}

D3DApp::D3DApp(HINSTANCE hInstance) : mhAppInst(hInstance) {
	// Only one D3DApp can be constructed.
	assert(mApp == nullptr); // nullptr resolves to false. thus, assert(false)
	mApp = this;
}

// destructor
D3DApp::~D3DApp() {
	OutputDebugString(L"D3DAPP DESTRUCTOR START\n");
	if (md3dDevice != nullptr)
		FlushCommandQueue();
	mApp = nullptr;
	OutputDebugString(L"D3DAPP DESTRUCTOR END\n");
}

// returns application instance handle
HINSTANCE D3DApp::AppInst()const {
	// HINSTANCE is a handle to a loaded module.
	return mhAppInst;
}

HWND D3DApp::MainWnd() const {
	// HWND is a handle to one specific window instance on screen
	return mhMainWnd;
}

float D3DApp::AspectRatio() const {
	return static_cast<float>(mClientWidth) / mClientHeight;
}

bool D3DApp::Get4xMsaaState()const {
	return m4xMsaaState;
}

void D3DApp::Set4xMsaaState(bool value) {
	if (m4xMsaaState != value)
	{
		m4xMsaaState = value;

		// Recreate the swapchain and buffers with new multisample settings.
		CreateSwapChain();
		OnResize();
	}
}

bool D3DApp::InitMainWindow()
{
	WNDCLASS wc;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = MainWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = mhAppInst;
	// wc.hIcon = LoadIcon(0, IDI_APPLICATION);
	wc.hIcon = LoadIcon(mhAppInst, MAKEINTRESOURCE(IDI_ICON1));

	wc.hCursor = LoadCursor(0, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
	wc.lpszMenuName = 0;
	wc.lpszClassName = L"MainWnd";

	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass Failed.", 0, 0);
		return false;
	}

	// Scale the hardcoded client-area defaults (mClientWidth/Height) by system DPI
	// so the window looks the same physical size regardless of display scaling.
	// Required after SetProcessDpiAwarenessContext(...PER_MONITOR_AWARE_V2) in
	// WinMain — pre-DPI-awareness, Windows bilinear-stretched a 1280x800 logical
	// window to ~1920x1200 physical on a 150% display; now those numbers are
	// taken literally, so the window appears 33% smaller without this fixup.
	// GetDpiForSystem returns the primary monitor's effective DPI (96 = 100%).
	const UINT  sysDpi   = GetDpiForSystem();
	const float dpiScale = (sysDpi > 0) ? sysDpi / 96.0f : 1.0f;
	mClientWidth  = static_cast<int>(mClientWidth  * dpiScale);
	mClientHeight = static_cast<int>(mClientHeight * dpiScale);

	// Compute window rectangle dimensions based on requested client area dimensions.
	RECT R = { 0, 0, mClientWidth, mClientHeight };
	AdjustWindowRect(&R, WS_OVERLAPPEDWINDOW, false);
	int width = R.right - R.left;
	int height = R.bottom - R.top;

	mhMainWnd = CreateWindow(L"MainWnd", mMainWndCaption.c_str(),
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, 0, 0, mhAppInst, 0);
	if (!mhMainWnd)
	{
		MessageBox(0, L"CreateWindow Failed.", 0, 0);
		return false;
	}

	ShowWindow(mhMainWnd, SW_SHOW);
	UpdateWindow(mhMainWnd);

	return true;
}

void D3DApp::CalculateFrameStats()
{
	// Code computes the average frames per second, and also the 
	// average time it takes to render one frame.  These stats 
	// are appended to the window caption bar.

	static int frameCnt = 0;
	static float timeElapsed = 0.0f;

	frameCnt++;

	// Compute averages over one second period.
	if ((mTimer.TotalTime() - timeElapsed) >= 1.0f)
	{
		float fps = (float)frameCnt; // fps = frameCnt / 1
		float mspf = 1000.0f / fps;

		// std::wstring fpsStr = std::to_wstring(fps);
		// std::wstring mspfStr = std::to_wstring(mspf);

		mFpsStr = std::to_wstring(static_cast<int>(fps));
		mMspfStr = std::to_wstring(static_cast<int>(mspf));

		// Reset for next average.
		frameCnt = 0;
		timeElapsed += 1.0f;
	}
}

int D3DApp::Run()
{
	MSG msg = { 0 };

	mTimer.Reset();

	while (msg.message != WM_QUIT)
	{
		// If there are Window messages then process them.
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		// Otherwise, do animation/game stuff.
		else
		{
			mTimer.Tick();

			if (!mAppPaused)
			{
				CalculateFrameStats();
				Update(mTimer);
				Draw(mTimer);
				if(capFPS) mTimer.LimitFPS(targetFPS);
			} 
			else
			{
				Sleep(100);
			}
		}
	}

	return (int)msg.wParam;
}

bool D3DApp::Initialize()
{
	if (!InitMainWindow())
		return false;

	if (!InitDirect3D())
		return false;

	// Do the initial resize code.
	OnResize();

	return true;
}

