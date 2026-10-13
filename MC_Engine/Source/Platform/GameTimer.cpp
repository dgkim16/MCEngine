//***************************************************************************************
// GameTimer.cpp by Frank Luna (C) 2011 All Rights Reserved.
//***************************************************************************************

#include "Platform/GameTimer.h"
#include "PIL/PerformanceTimer.h"

GameTimer::GameTimer()
	: mSecondsPerCount(0.0), mDeltaTime(-1.0), mBaseTime(0),
	mPausedTime(0), mPrevTime(0), mCurrTime(0), mStopped(false)
{
	const std::int64_t countsPerSec = MC::PIL::PerformanceTimer::TicksPerSecond();
	mSecondsPerCount = 1.0 / (double)countsPerSec;
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float GameTimer::TotalTime()const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if (mStopped)
	{
		return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
	}

	// The distance mCurrTime - mBaseTime includes paused time,
	// which we do not want to count.  To correct this, we can subtract 
	// the paused time from mCurrTime:  
	//
	//  (mCurrTime - mPausedTime) - mBaseTime 
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mCurrTime

	else
	{
		return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
	}
}

float GameTimer::DeltaTime()const
{
	return (float)mDeltaTime;
}

void GameTimer::Reset()
{
	const std::int64_t currTime = MC::PIL::PerformanceTimer::Ticks();

	mBaseTime = currTime;
	mPrevTime = currTime;
	mPausedTime = 0;	// pauses before the new base time are not part of TotalTime
	mStopTime = 0;
	mStopped = false;
}

void GameTimer::CalculateFrameStats()
{
	// Code computes the average frames per second, and also the
	// average time it takes to render one frame.  These stats
	// are appended to the window caption bar.

	mFrameCnt++;

	// Compute averages over one second period.
	if ((TotalTime() - mTimeElapsed) >= 1.0f)
	{
		float fps = (float)mFrameCnt; // fps = frameCnt / 1
		float mspf = 1000.0f / fps;

		mFpsStr = std::to_wstring(static_cast<int>(fps));
		mMspfStr = std::to_wstring(static_cast<int>(mspf));

		// Reset for next average.
		mFrameCnt = 0;
		mTimeElapsed += 1.0f;
	}
}

void GameTimer::Start()
{
	const std::int64_t startTime = MC::PIL::PerformanceTimer::Ticks();


	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	if (mStopped)
	{
		mPausedTime += (startTime - mStopTime);

		mPrevTime = startTime;
		mStopTime = 0;
		mStopped = false;
	}
}

void GameTimer::Stop()
{
	if (!mStopped)
	{
		const std::int64_t currTime = MC::PIL::PerformanceTimer::Ticks();

		mStopTime = currTime;
		mStopped = true;
	}
}

void GameTimer::Tick()
{
	if (mStopped)
	{
		mDeltaTime = 0.0;
		return;
	}

	mCurrTime = MC::PIL::PerformanceTimer::Ticks();

	// Time difference between this frame and the previous.
	mDeltaTime = (mCurrTime - mPrevTime) * mSecondsPerCount;

	// Prepare for next frame.
	mPrevTime = mCurrTime;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (mDeltaTime < 0.0)
	{
		mDeltaTime = 0.0;
	}
}

float GameTimer::GetmSecondsPerCount() {
	return static_cast<float>(mSecondsPerCount);
}

using clock_type = std::chrono::steady_clock;

void GameTimer::LimitFPS(double targetFPS)
{
	using namespace std::chrono;

	if (targetFPS <= 0.0)
		return;

	static bool initialized = false;
	static clock_type::time_point nextFrameTime{};

	const auto targetFrameTime =
		duration_cast<clock_type::duration>(duration<double>(1.0 / targetFPS));

	auto now = clock_type::now();

	if (!initialized)
	{
		nextFrameTime = now + targetFrameTime;
		initialized = true;
		return;
	}

	if (now < nextFrameTime)
	{
		const auto coarseSleepMargin = 1ms;
		auto sleepUntilTime = nextFrameTime - coarseSleepMargin;

		if (sleepUntilTime > now)
		{
			std::this_thread::sleep_until(sleepUntilTime);
		}

		while (clock_type::now() < nextFrameTime)
		{
		}
	}
	else
	{
		// We are late. If we missed by a lot, resync so drift does not accumulate.
		if (now - nextFrameTime > targetFrameTime * 4)
		{
			nextFrameTime = now;
		}
	}

	nextFrameTime += targetFrameTime;
}