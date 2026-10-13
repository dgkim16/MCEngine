#include "PIL/Display.h"
#include <windows.h>

namespace MC::PIL {

DisplayInfo QueryDisplay(void* nativeWindow)
{
	DisplayInfo info;
	if (!nativeWindow)
		return info;
	HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(nativeWindow), MONITOR_DEFAULTTONEAREST);
	info.display = monitor;
	MONITORINFOEXW mi{};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(monitor, &mi))
	{
		DEVMODEW dm{};
		dm.dmSize = sizeof(dm);
		// 0 and 1 both mean "the hardware default rate": no number to report.
		if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			info.refreshHz = static_cast<int>(dm.dmDisplayFrequency);
	}
	return info;
}

}  // namespace MC::PIL
