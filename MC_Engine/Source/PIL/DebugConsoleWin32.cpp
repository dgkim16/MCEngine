#include "PIL/DebugConsole.h"
#include <windows.h>
#include <cstdio>
#include <iostream>

namespace MC::PIL {

void CreateDebugConsole(void* instance, int iconId)
{
	AllocConsole();

	FILE* fp;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	freopen_s(&fp, "CONIN$", "r", stdin);

	const HINSTANCE hInstance = static_cast<HINSTANCE>(instance);
	HWND hConsoleWnd = GetConsoleWindow();
	HICON hIconBig = (HICON)LoadImage(
		hInstance,
		MAKEINTRESOURCE(iconId),
		IMAGE_ICON,
		32, 32,
		LR_DEFAULTCOLOR
	);

	HICON hIconSmall = (HICON)LoadImage(
		hInstance,
		MAKEINTRESOURCE(iconId),
		IMAGE_ICON,
		16, 16,
		LR_DEFAULTCOLOR
	);
	SendMessage(hConsoleWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
	SendMessage(hConsoleWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

	SetWindowTextW(hConsoleWnd, L"MC Engine Debug Console");

	std::cout << "NEVER CLOSE THIS WINDOW BEFORE THE ACTUAL APP!!!!" << std::endl;
}

}  // namespace MC::PIL
