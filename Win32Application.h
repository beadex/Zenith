#pragma once

#include "D3D12Application.h"
#include "Timer.h"

class Win32Application
{
public:
	static int Run(D3D12Application* pApplication, HINSTANCE hInstance, int nCmdShow);
	static HWND GetHwnd() { return m_hWnd; }

protected:
	static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	static Timer m_timer;

	static bool m_appPaused;
private:
	static HMENU CreateApplicationMenu();
	static HWND m_hWnd;
};