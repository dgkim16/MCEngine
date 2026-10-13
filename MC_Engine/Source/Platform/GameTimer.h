//***************************************************************************************
// GameTimer.h by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#ifndef GAMETIMER_H
#define GAMETIMER_H

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

class GameTimer
{
public:
	GameTimer();

	float TotalTime()const; // in seconds
	float DeltaTime()const; // in seconds

	void Reset(); // Call before message loop.
	void Start(); // Call when unpaused.
	void Stop();  // Call when paused.
	void Tick();  // Call every frame.
	float GetmSecondsPerCount();
	void LimitFPS(double targetFPS);

	// FPS cap, applied by PlatformSystem::LimitFrameRate. PlatformSystem sets maxFPS and
	// targetFPS to the refresh rate of the window's monitor; the UI edits capFPS and targetFPS.
	bool capFPS = false;
	double targetFPS = 60.0;
	int maxFPS = 60;

	// Frame statistics for the window title, averaged over one second. PlatformSystem::Tick
	// calls CalculateFrameStats once per frame while the app is not paused.
	void CalculateFrameStats();
	const std::wstring& FpsText() const { return mFpsStr; }
	const std::wstring& MspfText() const { return mMspfStr; }

private:
	double mSecondsPerCount;
	double mDeltaTime;

	// Readings of PIL::PerformanceTimer::Ticks().
	std::int64_t mBaseTime;
	std::int64_t mPausedTime;
	std::int64_t mStopTime;
	std::int64_t mPrevTime;
	std::int64_t mCurrTime;

	bool mStopped;

	int mFrameCnt = 0;			// frames since mTimeElapsed
	float mTimeElapsed = 0.0f;	// TotalTime() at which the current one-second average started
	std::wstring mFpsStr = L"mFpsStr";
	std::wstring mMspfStr = L"mMspfStr";
};

#endif // GAMETIMER_H