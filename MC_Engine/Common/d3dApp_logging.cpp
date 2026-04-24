#include "d3dApp.h"
#include <WindowsX.h>
#include <optional>
#include <dxgi1_6.h>

void D3DApp::LogAdapters()
{
	UINT i = 0;
	IDXGIAdapter* adapter = nullptr;
	std::vector<IDXGIAdapter*> adapterList;
	while (mdxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		std::wstring text = L"***Adapter: ";
		text += desc.Description;
		text += L"\n";

		OutputDebugString(text.c_str());

		adapterList.push_back(adapter);

		++i;
	}

	for (size_t i = 0; i < adapterList.size(); ++i)
	{
		LogAdapterOutputs(adapterList[i]);
		ReleaseCom(adapterList[i]);
	}
}

void D3DApp::LogAdapterOutputs(IDXGIAdapter* adapter)
{
	UINT i = 0;
	IDXGIOutput* output = nullptr;
	while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_OUTPUT_DESC desc;
		output->GetDesc(&desc);

		std::wstring text = L"***Output: ";
		text += desc.DeviceName;
		text += L"\n";
		OutputDebugString(text.c_str());

		LogOutputDisplayModes(output, mBackBufferFormat);

		ReleaseCom(output);

		++i;
	}
}

void D3DApp::LogOutputDisplayModes(IDXGIOutput* output, DXGI_FORMAT format)
{
	UINT count = 0;
	UINT flags = 0;

	// Call with nullptr to get list count.
	output->GetDisplayModeList(format, flags, &count, nullptr);

	std::vector<DXGI_MODE_DESC> modeList(count);
	output->GetDisplayModeList(format, flags, &count, &modeList[0]);

	for (auto& x : modeList)
	{
		UINT n = x.RefreshRate.Numerator;
		UINT d = x.RefreshRate.Denominator;
		std::wstring text =
			L"Width = " + std::to_wstring(x.Width) + L" " +
			L"Height = " + std::to_wstring(x.Height) + L" " +
			L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) +
			L"\n";

		::OutputDebugString(text.c_str());
	}
}

using Microsoft::WRL::ComPtr;

std::optional<double> GetCurrentWindowMonitorRefreshRate(HWND hwnd)
{
	HMONITOR targetMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (!targetMonitor)
		return std::nullopt;

	ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return std::nullopt;

	for (UINT adapterIndex = 0;; ++adapterIndex)
	{
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;

		for (UINT outputIndex = 0;; ++outputIndex)
		{
			ComPtr<IDXGIOutput> output;
			if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_OUTPUT_DESC desc{};
			if (FAILED(output->GetDesc(&desc)))
				continue;

			if (desc.Monitor != targetMonitor)
				continue;

			MONITORINFOEXW mi{};
			mi.cbSize = sizeof(mi);
			if (!GetMonitorInfoW(targetMonitor, &mi))
				return std::nullopt;

			DEVMODEW dm{};
			dm.dmSize = sizeof(dm);

			if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
				return std::nullopt;

			if (dm.dmDisplayFrequency == 0 || dm.dmDisplayFrequency == 1)
				return std::nullopt;

			return static_cast<double>(dm.dmDisplayFrequency);
		}
	}

	return std::nullopt;
}


static HMONITOR gCurrentMonitor = nullptr;
void D3DApp::UpdateCurrentMonitorRefreshRate(HWND hwnd) {
	HMONITOR newMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (newMonitor != gCurrentMonitor)
	{
		gCurrentMonitor = newMonitor;
		MONITORINFOEXW mi{};
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoW(gCurrentMonitor, &mi))
		{
			OutputDebugStringW(L"Monitor changed.\n");
			OutputDebugStringW(mi.szDevice);
			OutputDebugStringW(L"\n");
		}
		// Requery refresh rate here if needed.
		// Rebuild swapchain settings here if needed.
		std::optional<double> opt_d = GetCurrentWindowMonitorRefreshRate(hwnd);
		if (opt_d.has_value()) {
			std::wstring msg = L"new monitor has fps of " + std::to_wstring((int)opt_d.value());
			OutputDebugString(msg.c_str());
			maxFPS = (int)opt_d.value();
			targetFPS = (double)maxFPS;
		}
	}
}