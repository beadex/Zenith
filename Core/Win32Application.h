#pragma once

#include "D3D12Application.h"
#include "Util/Timer.h"

class Win32Application
{
public:
  // Static-only helper that owns the single Win32 window and message loop used
	// by this sample application.
	static int Run(D3D12Application* pApplication, HINSTANCE hInstance, int nCmdShow);
	static HWND GetHwnd() { return m_hWnd; }
	static void RequestRender();

protected:
	static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	static Timer m_timer;

	static bool m_appPaused;
    static bool m_renderRequested;
private:
	static HMENU CreateApplicationMenu();
	static HWND m_hWnd;
};