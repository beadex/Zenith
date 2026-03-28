#include "pch.h"
#include "Win32Application.h"

HWND Win32Application::m_hWnd = nullptr;
Timer Win32Application::m_timer = Timer();
bool Win32Application::m_appPaused = false;

int Win32Application::Run(D3D12Application* pApp, HINSTANCE hInstance, int nCmdShow) {
	int argc;

	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	pApp->ParseCommandLineArgs(argv, argc);
	LocalFree(argv);

	// Initialize the window class.
	WNDCLASSEX windowClass = { 0 };
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WndProc;
	windowClass.hInstance = hInstance;
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.lpszClassName = L"ZenithWndClass";
	RegisterClassEx(&windowClass);

	// Create the window's client area and adjust the window size accordingly.
	// NOTE: No fullscreen support
	RECT windowRect = { 0, 0, static_cast<LONG>(pApp->GetWidth()), static_cast<LONG>(pApp->GetHeight()) };
	AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	// Create the window and store a handle to it.
	m_hWnd = CreateWindow(
		windowClass.lpszClassName,
		pApp->GetTitle(),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr,        // We have no parent window.
		nullptr,        // We aren't using menus.
		hInstance,
		pApp);

	// Initialize the D3D App. OnInit is defined in each child-implementation of D3DApp.
	pApp->OnInit();
	
	// Show the window and set it as the foreground window.
	ShowWindow(m_hWnd, nCmdShow);
	SetForegroundWindow(m_hWnd);
	SetFocus(m_hWnd);

	// Main loop.
	MSG msg = {};

	m_timer.Reset();

	while (msg.message != WM_QUIT)
	{
		// Process any messages in the queue.
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			m_timer.Tick();

			if (!m_appPaused) {
				pApp->OnUpdate(m_timer);
				pApp->OnRender(m_timer);
			}
			else
			{
				Sleep(100);
			}
		}
	}

	pApp->OnDestroy();

	// Return this part of the WM_QUIT message to Windows.
	return static_cast<char>(msg.wParam);
}

LRESULT CALLBACK Win32Application::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	D3D12Application* pApp = reinterpret_cast<D3D12Application*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	switch (message)
	{
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			m_appPaused = true;
			m_timer.Stop();
		}
		else
		{
			m_appPaused = false;
			m_timer.Start();
		}
		return 0;

	case WM_CREATE:
	{
		LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
	}
	return 0;

	case WM_KEYDOWN:
		if (pApp)
		{
			pApp->OnKeyDown(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_KEYUP:
		if (pApp)
		{
			pApp->OnKeyUp(static_cast<UINT8>(wParam));
		}
		return 0;

	case WM_ENTERSIZEMOVE:
		m_appPaused = true;
		m_timer.Stop();
		return 0;

	case WM_EXITSIZEMOVE:
		m_appPaused = false;
		m_timer.Start();
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}