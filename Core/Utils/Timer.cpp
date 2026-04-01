#include "../pch.h"
#include "Timer.h"

Timer::Timer() :
	m_secondsPerCount(0.0),
	m_deltaTime(-1.0),
	m_baseTime(0),
	m_pausedTime(0),
	m_prevTime(0),
	m_currTime(0),
	m_stopped(false)
{
	UINT64 countsPerSec;
	QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
	m_secondsPerCount = 1.0 / (double)countsPerSec;
}

// Returns the total time elapsed since Reset() was called, NOT counting any
// time when the clock is stopped.
float Timer::TotalTime()const
{
	// If we are stopped, do not count the time that has passed since we stopped.
	// Moreover, if we previously already had a pause, the distance 
	// mStopTime - mBaseTime includes paused time, which we do not want to count.
	// To correct this, we can subtract the paused time from mStopTime:  
	//
	//                     |<--paused time-->|
	// ----*---------------*-----------------*------------*------------*------> time
	//  mBaseTime       mStopTime        startTime     mStopTime    mCurrTime

	if (m_stopped)
	{
		return (float)(((m_stopTime - m_pausedTime) - m_baseTime) * m_secondsPerCount);
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
		return (float)(((m_currTime - m_pausedTime) - m_baseTime) * m_secondsPerCount);
	}
}

float Timer::DeltaTime()const
{
	return (float)m_deltaTime;
}

void Timer::Reset()
{
	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

	m_baseTime = currTime;
	m_prevTime = currTime;
	m_stopTime = 0;
	m_stopped = false;
}

void Timer::Start()
{
	__int64 startTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&startTime);


	// Accumulate the time elapsed between stop and start pairs.
	//
	//                     |<-------d------->|
	// ----*---------------*-----------------*------------> time
	//  mBaseTime       mStopTime        startTime     

	if (m_stopped)
	{
		m_pausedTime += (startTime - m_stopTime);

		m_prevTime = startTime;
		m_stopTime = 0;
		m_stopped = false;
	}
}

void Timer::Stop()
{
	if (!m_stopped)
	{
		__int64 currTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

		m_stopTime = currTime;
		m_stopped = true;
	}
}

void Timer::Tick()
{
	if (m_stopped)
	{
		m_deltaTime = 0.0;
		return;
	}

	__int64 currTime;
	QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
	m_currTime = currTime;

	// Time difference between this frame and the previous.
	m_deltaTime = (m_currTime - m_prevTime) * m_secondsPerCount;

	// Prepare for next frame.
	m_prevTime = m_currTime;

	// Force nonnegative.  The DXSDK's CDXUTTimer mentions that if the 
	// processor goes into a power save mode or we get shuffled to another
	// processor, then mDeltaTime can be negative.
	if (m_deltaTime < 0.0)
	{
		m_deltaTime = 0.0;
	}
}
