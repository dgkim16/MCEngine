#include "Platform/PlatformSystem.h"
#include "PIL/EventPump.h"
#include "PIL/Display.h"
#include <chrono>
#include <thread>

namespace MC {

PlatformSystem::PlatformSystem(void* instance, const wchar_t* title, int clientWidth, int clientHeight, int iconId)
	: mTitle(title), mTimer(),
	// The window raises events while it is being created; every member they touch is above it.
	mWindow(instance, mTitle, clientWidth, clientHeight, iconId,
		[this](const PIL::WindowEvent& event) { return OnWindowEvent(event); })
{
}

PlatformSystem::~PlatformSystem() = default;	// mWindow destroys the window

// =====================================================================
// ========================= PUBLIC FUNCTIONS =========================


bool PlatformSystem::DispatchOneMessage()
{
	int exitCode = 0;
	switch (PIL::PumpOneEvent(exitCode))
	{
	case PIL::PumpResult::Empty:
		return false;
	case PIL::PumpResult::Quit:
		mRunning = false;
		mExitCode = exitCode;
		return true;
	case PIL::PumpResult::Handled:
		return true;
	}
	return true;
}

void PlatformSystem::SleepWhilePaused()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(100)); // coco nen neh
}

void PlatformSystem::SetPauseOnFocusLoss(bool pause)
{
	mPauseOnFocusLoss = pause;
	// A deactivation before this call may have paused the app, and no activation will come
	// to undo it while nobody uses the window. End that pause unless something else holds it.
	if (!pause && mAppPaused && !mMinimized && !mResizing)
	{
		mAppPaused = false;
		mTimer.Start();
	}
}

bool PlatformSystem::TakeResizeRequest(int& width, int& height)
{
	if (!mResizeRequested)
		return false;
	if (mMinimized || ClientWidth() == 0 || ClientHeight() == 0)
		return false;
	mResizeRequested = false;
	width = ClientWidth();	// hand the caller the size to resize to
	height = ClientHeight();
	return true;
}

void PlatformSystem::SetTitleSuffix(const std::wstring& suffix)
{
	mWindow.SetTitle(mTitle + suffix);
}

void PlatformSystem::RequestClose()
{
	mWindow.RequestClose();	// same effect as pressing X
}

bool PlatformSystem::TakeCloseRequest()
{
	const bool requested = mCloseRequested;	// was a close swallowed since the last call?
	mCloseRequested = false;	// taken: the next call returns false until a new close arrives
	return requested;
}

void PlatformSystem::ResetClock()
{
	mTimer.Reset();
}

float PlatformSystem::Tick()
{
	mTimer.Tick();
	if (!mAppPaused)
	{
		mTimer.CalculateFrameStats();	// the stats count frames that render
		// Publish input only for a frame that will run. While paused, events stay pending
		// (a focus-loss release among them) and reach the first frame after the pause.
		mInput.NewFrame();
	}
	return mTimer.DeltaTime();
}

void PlatformSystem::LimitFrameRate()
{
	if (mTimer.capFPS)
		mTimer.LimitFPS(mTimer.targetFPS);
}

// =====================================================================
// ========================= PRIVATE FUNCTIONS =========================

bool PlatformSystem::OnWindowEvent(const PIL::WindowEvent& event)
{
	using Type = PIL::WindowEvent::Type;
	switch (event.type) {
	// ============= PAUSING =============
	case Type::Deactivated:
		if (!mPauseOnFocusLoss)
			break;	// keep running in the background (SetPauseOnFocusLoss(false))
		mAppPaused = true;
		mTimer.Stop();
		break;
	case Type::Activated:
		mAppPaused = false;
		mTimer.Start();
		break;
	case Type::EnterSizeMove:
		mResizing = true;
		mTimer.Stop();
		mAppPaused = true;
		break;
	case Type::ExitSizeMove:
		mResizing = false;
		mTimer.Start();
		mAppPaused = false;
		mResizeRequested = true;	// request resize at the end of resize, not during resize
		break;
	// ============= Resizing =============
	case Type::Minimized:
		mTimer.Stop();
		mAppPaused = true;
		mMinimized = true;
		mMaximized = false;
		break;
	case Type::Maximized:
		mTimer.Start();
		mAppPaused = false;
		mResizeRequested = true;
		mMinimized = false;
		mMaximized = true;
		break;
	case Type::Restored:
		if (mMinimized) {
			mMinimized = false;
			mTimer.Start();
			mAppPaused = false;
		}
		mMaximized = false;
		if (!mResizing)
			mResizeRequested = true;
		break;
	case Type::DisplayChanged:
		UpdateCurrentMonitorRefreshRate();
		break;
	// ============= Closing =============
	case Type::CloseRequested:
		if (mConfirmClose) {
			mCloseRequested = true;
			return true;	// keep the window open; TakeCloseRequest hands the request on
		}
		break;
	case Type::Destroyed:
		break;	// the PIL posts the quit request; DispatchOneMessage sees it
	// ============= INPUT =============
	case Type::Input:
		mInput.OnEvent(event.input);
		break;
	// Keyboard focus moved to another window: its key-ups and button-ups will not come here.
	case Type::FocusLost:
		mInput.OnFocusLost();
		break;
	}
	return false;
}

void PlatformSystem::UpdateCurrentMonitorRefreshRate()
{
	const PIL::DisplayInfo display = PIL::QueryDisplay(mWindow.NativeHandle());
	if (display.display == mCurrentMonitor)
		return;	// still on the same display: keep the user's target
	mCurrentMonitor = display.display;
	if (display.refreshHz > 1)
	{
		mTimer.maxFPS = display.refreshHz;
		mTimer.targetFPS = static_cast<double>(display.refreshHz);
	}
}

}  // namespace MC
