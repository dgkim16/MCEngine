#include "MCEngine.h"
#include "Resource.h"

// For debugging via std::cout
void CreateDebugConsole(HINSTANCE hInstance)
{
	AllocConsole();

	FILE* fp;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	freopen_s(&fp, "CONIN$", "r", stdin);

	HWND hConsoleWnd = GetConsoleWindow();
	HICON hIconBig = (HICON)LoadImage(
		hInstance,
		MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON,
		32, 32,
		LR_DEFAULTCOLOR
	);

	HICON hIconSmall = (HICON)LoadImage(
		hInstance,
		MAKEINTRESOURCE(IDI_ICON1),
		IMAGE_ICON,
		16, 16,
		LR_DEFAULTCOLOR
	);
	SendMessage(hConsoleWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
	SendMessage(hConsoleWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
	
	std::wstring windowText = L"MC Engine Debug Console";
	SetWindowText(hConsoleWnd, windowText.c_str());

	std::cout << "NEVER CLOSE THIS WINDOW BEFORE THE ACTUAL APP!!!!" << std::endl;
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
	PSTR cmdLine, int showCmd){
	// Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	CreateDebugConsole(hInstance);

	try
	{
		MCEngine theApp(hInstance);
		if (!theApp.Initialize())
			return 0;

		return theApp.Run();
	}
	catch (DxException& e)
	{
		MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
		return 0;
	}
}