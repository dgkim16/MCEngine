#include "Legacy/d3dApp.h"
#include "Core/LoggingSystem.h"

// destructor
D3DApp::~D3DApp() {
	MC::LoggingSystem::Debug("D3DAPP DESTRUCTOR START");
	if (md3dDevice != nullptr)
		FlushCommandQueue();
	MC::LoggingSystem::Debug("D3DAPP DESTRUCTOR END");
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

bool D3DApp::Initialize(HWND window, int clientWidth, int clientHeight)
{
	mhMainWnd = window;
	mClientWidth = clientWidth;
	mClientHeight = clientHeight;

	// Device, queue and swap chain only. MCEngine::Initialize creates the RTV/DSV heaps and
	// does the first resize itself, so nothing here calls back into the derived class.
	return InitDirect3D();
}

