#pragma once

namespace MC::PIL {

// @brief	A display (a native handle; Windows: HMONITOR) and its refresh rate in Hz, 0 when the
// @brief	OS reports none.
struct DisplayInfo {
	void* display = nullptr;
	int refreshHz = 0;
};

// @brief	The display a native window is mostly on (Windows: the HWND), and its refresh rate.
// @brief	A null window gives an empty DisplayInfo.
DisplayInfo QueryDisplay(void* nativeWindow);

}  // namespace MC::PIL
