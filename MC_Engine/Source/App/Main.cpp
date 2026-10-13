#include "Legacy/MCEngine.h"
#include "App/Resource.h"
#include "PIL/DebugConsole.h"
#include <cstdio>
#include <cstdlib>
#include <exception>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd){
	// Per-monitor v2 DPI awareness — must run BEFORE any window is created so the
	// font atlas rasterizes at native pixel density and Windows stops bilinear-
	// stretching the backbuffer. Available since Windows 10 1607; in user32.dll,
	// no extra link required.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	MC::PIL::CreateDebugConsole(hInstance, IDI_ICON1);	// for std::cout debugging

	try
	{
		MCEngine theApp(hInstance, IDI_ICON1);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (const std::exception& e)	// DxException is a std::runtime_error
	{
#if defined(_DEBUG)
		// A harness run (--capture-sweep, --run-tests) sends CRT error reports to
		// stderr, which --verify-log makes its log: report there and exit instead
		// of blocking on a dialog nobody is there to close.
		if (_set_error_mode(_REPORT_ERRMODE) == _OUT_TO_STDERR) {
			std::fprintf(stderr, "Fatal error: %s\n", e.what());
			std::fflush(stderr);
			return 1;
		}
#endif
		MessageBoxA(nullptr, e.what(), "MC Engine - fatal error", MB_OK | MB_ICONERROR);
		return 1;
	}
}