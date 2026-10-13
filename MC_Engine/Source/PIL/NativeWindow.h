#pragma once
#include "PIL/WindowEvent.h"
#include <cstdint>
#include <functional>
#include <string>

namespace MC::PIL {

// @brief	Owns one OS window: creates and shows it, destroys it, sets its title, asks it to
// @brief	close, and reports its handle and client size. Its OS callback hands every message
// @brief	to the native hook, decodes it (DecodeWindowMessage) and raises the WindowEvent.
// @brief	This header is OS-neutral; NativeWindowWin32.cpp implements it for Windows.
class NativeWindow {
public:
	// @brief	Receives every decoded event. Returning true suppresses the OS default handling;
	// @brief	only CloseRequested uses that (the window then stays open).
	using EventHandler = std::function<bool(const WindowEvent& event)>;
	// @brief	Sees every native event before it is decoded (Windows: the window procedure's
	// @brief	hwnd, message, wParam, lParam). Returning true consumes it. Destroy events cannot be
	// @brief	consumed. Exists for ImGui's platform backend.
	using NativeHook = std::function<bool(void* window, unsigned message, std::uintptr_t wParam, std::intptr_t lParam)>;

	// @brief	Creates and shows the window; throws if that fails. The client size is given at
	// @brief	100% scaling and is scaled by the display's DPI. instance is the native application
	// @brief	handle (Windows: the HINSTANCE) and iconId the icon resource. handler is installed
	// @brief	before the window exists, because the OS sends events during creation.
	NativeWindow(void* instance, const std::wstring& title, int clientWidth, int clientHeight, int iconId, EventHandler handler);
	// @brief	Destroys the window if it still exists and releases what the constructor registered.
	~NativeWindow();
	NativeWindow(const NativeWindow&) = delete;
	NativeWindow& operator=(const NativeWindow&) = delete;

	// @brief	Installs the hook that sees every native event first (see NativeHook).
	void SetNativeHook(NativeHook hook) { mNativeHook = std::move(hook); }
	// @brief	The native window handle (Windows: the HWND). Null once the window is destroyed.
	void* NativeHandle() const { return mHandle; }
	// @brief	The current client-area size in pixels.
	int ClientWidth() const { return mClientWidth; }
	int ClientHeight() const { return mClientHeight; }
	// @brief	Sets the title-bar text.
	void SetTitle(const std::wstring& title);
	// @brief	Asks the window to close, as its close button does (raises CloseRequested later).
	void RequestClose();

private:
	// @brief	Defined in the OS implementation file: the OS-side window callback, which calls
	// @brief	HandleMessage.
	friend struct WindowProcedure;
	// @brief	Handles one native message: hook first, then decode and raise. Returns the OS
	// @brief	callback's result.
	std::intptr_t HandleMessage(void* window, unsigned message, std::uintptr_t wParam, std::intptr_t lParam);
	bool Raise(const WindowEvent& event) { return mHandler && mHandler(event); }

	void* mInstance = nullptr;	// native application handle (Windows: HINSTANCE)
	void* mHandle = nullptr;	// native window handle (Windows: HWND)
	int mClientWidth = 0;
	int mClientHeight = 0;
	EventHandler mHandler;
	NativeHook mNativeHook;
};

}  // namespace MC::PIL
