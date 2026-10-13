#include "PIL/EventPump.h"
#include <windows.h>

namespace MC::PIL {

PumpResult PumpOneEvent(int& exitCode)
{
	MSG msg = {};
	if (!PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
		return PumpResult::Empty;
	if (msg.message == WM_QUIT)	// belongs to no window, so it is never dispatched
	{
		exitCode = static_cast<int>(msg.wParam);
		return PumpResult::Quit;
	}
	TranslateMessage(&msg);	// posts an extra WM_CHAR for the typed character
	DispatchMessage(&msg);	// calls the procedure of msg.hwnd (NativeWindow's WindowProcedure::Proc)
	return PumpResult::Handled;
}

}  // namespace MC::PIL
