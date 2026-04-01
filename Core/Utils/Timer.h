#pragma once

class Timer
{
public:
	Timer();

	float TotalTime() const; // in seconds
	float DeltaTime() const; // in seconds

	void Reset(); // Call before message loop
	void Start(); // Call when unpaused.
	void Stop(); // Call when pause.
	void Tick(); // Call every frame.

private:
	double m_secondsPerCount;
	double m_deltaTime;

	UINT64 m_baseTime;
	UINT64 m_pausedTime;
	UINT64 m_stopTime;
	UINT64 m_prevTime;
	UINT64 m_currTime;

	bool m_stopped;
};