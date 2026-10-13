#pragma once
#include "Platform/GameTimer.h"
#include "Platform/Input.h"
#include "PIL/NativeWindow.h"
#include "Core/LoggingSystem.h"
#include <cstdint>
#include <functional>
#include <string>

namespace MC {

// @brief	The application's platform state: the window (through PIL::NativeWindow), the OS event
// @brief	pump (through PIL::PumpOneEvent), the pause/resize/close state, input and the frame
// @brief	clock. OS-independent: it reacts to decoded PIL::WindowEvents and never calls the OS.
class PlatformSystem {
public:
	// @brief	Sees every native window event before PlatformSystem does. Returning true
	// @brief	consumes the event, and PlatformSystem does not handle it. Destroy events cannot
	// @brief	be consumed: PlatformSystem always handles them.
	// @brief	The arguments are the native event (Windows: the window procedure's hwnd,
	// @brief	message, wParam and lParam).
	using MessageHook = PIL::NativeWindow::NativeHook;

	// @brief	Creates and shows the application window; throws if that fails. The client size
	// @brief	is given at 100% scaling and is scaled by the display's DPI. instance is the
	// @brief	native application handle (Windows: the HINSTANCE) and iconId the icon resource.
	PlatformSystem(void* instance, const wchar_t* title, int clientWidth, int clientHeight, int iconId);
	// @brief	Destroys the window if it still exists and releases what the constructor registered.
	~PlatformSystem();
	PlatformSystem(const PlatformSystem&) = delete;
	PlatformSystem& operator=(const PlatformSystem&) = delete;

	// ---- Event pump ----
	// @brief	Handles one queued OS event. Returns false when the queue is empty.
	bool DispatchOneMessage();
	// @brief	False once the OS has asked the application to quit.
	bool IsRunning() const { return mRunning; }
	// @brief	The exit code that came with the quit request.
	int ExitCode() const { return mExitCode; }
	// @brief	Installs the hook that sees every native window event first (see MessageHook).
	void SetMessageHook(MessageHook hook) { mWindow.SetNativeHook(std::move(hook)); }

	// ---- Window state ----
	// @brief	True while the window is deactivated, minimized, or being dragged.
	bool IsPaused() const { return mAppPaused; }
	// @brief	Sleeps briefly, so a paused application does not spin the CPU.
	void SleepWhilePaused();
	// @brief	Whether losing focus pauses the application (default true). With false, the app
	// @brief	keeps running in the background, as unattended runs need; a pause that focus loss
	// @brief	already caused ends now. Minimizing and dragging still pause.
	void SetPauseOnFocusLoss(bool pause);
	// @brief	True once after the window asked for new buffers (maximize, restore, the end of a
	// @brief	drag, a programmatic resize); width and height get the client size. Never true
	// @brief	while the window is minimized or its client area is 0 wide or high: the request waits.
	bool TakeResizeRequest(int& width, int& height);
	// @brief	The native window handle the renderer presents to (Windows: the HWND).
	// @brief	Null once the window is destroyed.
	void* NativeWindow() const { return mWindow.NativeHandle(); }
	// @brief	The client area's width in pixels.
	int ClientWidth() const { return mWindow.ClientWidth(); }
	// @brief	The client area's height in pixels.
	int ClientHeight() const { return mWindow.ClientHeight(); }
	// @brief	Sets the window title to title + suffix.
	void SetTitleSuffix(const std::wstring& suffix);

	// ---- Close ----
	// @brief	Asks the window to close, as its close button does. Closing destroys the window,
	// @brief	which ends the loop, unless SetConfirmClose(true) is set.
	void RequestClose();
	// @brief	When true, a close only raises a request that TakeCloseRequest hands to whoever
	// @brief	must confirm it, instead of destroying the window.
	void SetConfirmClose(bool confirm) { mConfirmClose = confirm; }
	// @brief	True once after a close arrived while SetConfirmClose(true) was set.
	bool TakeCloseRequest();

	// ---- Frame clock ----
	// @brief	Resets the timer. Call right before the loop, so the first delta excludes start-up.
	void ResetClock();
	// @brief	Once per loop iteration that is not an event: ticks the timer and updates the
	// @brief	frame stats unless paused. Returns the delta in seconds.
	float Tick();
	// @brief	Seconds since ResetClock, excluding paused time.
	float TotalTime() const { return mTimer.TotalTime(); }
	// @brief	Waits out the rest of the frame when the timer's FPS cap is on.
	void LimitFrameRate();
	// @brief	The frame timer, for the FPS-cap controls and the frame stats.
	GameTimer& Timer() { return mTimer; }

	// ---- Input ----
	Input& GetInput() { return mInput; }
	const Input& GetInput() const { return mInput; }

private:
	// @brief	Reacts to one decoded window event: the pause, resize, close and input state.
	// @brief	Returns true to keep the OS default from running (a confirm-close only).
	bool OnWindowEvent(const PIL::WindowEvent& event);
	// @brief	Points the FPS cap at the refresh rate of the display the window is on, when the
	// @brief	window has moved to another display.
	void UpdateCurrentMonitorRefreshRate();

	std::wstring mTitle;
	GameTimer mTimer;
	MC::Input mInput;

	bool mRunning = true;
	int mExitCode = 0;
	bool mAppPaused = false;	// is the application paused?
	bool mPauseOnFocusLoss = true;	// does deactivation pause it? (SetPauseOnFocusLoss)
	bool mMinimized = false;	// is the application minimized?
	bool mMaximized = false;	// is the application maximized?
	bool mResizing = false;		// are the resize bars being dragged?
	bool mResizeRequested = false;
	bool mConfirmClose = false;
	bool mCloseRequested = false;
	void* mCurrentMonitor = nullptr;	// native display handle whose refresh rate the FPS cap follows (Windows: HMONITOR)

	// Last member: the window sends events during its own construction, and OnWindowEvent
	// touches every member above, so they must exist first.
	PIL::NativeWindow mWindow;
};

}  // namespace MC
