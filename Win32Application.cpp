#include "pch.h"
#include "Win32Application.h"

HWND  Win32Application::m_hWnd = nullptr;
Timer Win32Application::m_timer = Timer();
bool  Win32Application::m_appPaused = false;
bool  Win32Application::m_renderRequested = false;

HMENU Win32Application::CreateApplicationMenu()
{
	HMENU menuBar = CreateMenu();
	HMENU fileMenu = CreatePopupMenu();
	HMENU renderMenu = CreatePopupMenu();
	HMENU viewMenu = CreatePopupMenu();
	HMENU helpMenu = CreatePopupMenu();

	// The app creates its menu in code at runtime instead of using the .rc menu.
	// That means new items must be added here or they will not appear in the window.
	AppendMenu(fileMenu, MF_STRING, IDM_FILE_LOAD_MODEL, L"Load Model...");
	AppendMenu(fileMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(fileMenu, MF_STRING, IDM_EXIT, L"Exit");
	AppendMenu(renderMenu, MF_STRING, IDM_RENDER_IMAGE, L"Render Image...");
	AppendMenu(viewMenu, MF_STRING, IDM_VIEW_DIRECTIONAL_LIGHT, L"Directional Light...");
	AppendMenu(viewMenu, MF_STRING, IDM_VIEW_SOLID_GROUND_PLANE, L"Solid Ground Plane");
	AppendMenu(viewMenu, MF_STRING, IDM_VIEW_ADD_POINT_LIGHT, L"Add Point Light...");
	AppendMenu(helpMenu, MF_STRING, IDM_ABOUT, L"About");

	AppendMenu(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"Files");
	AppendMenu(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(renderMenu), L"Render");
	AppendMenu(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"View");
	AppendMenu(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"Help");

	return menuBar;
}

int Win32Application::Run(D3D12Application* pApp, HINSTANCE hInstance, int nCmdShow)
{
	int     argc;
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

	HMENU menuBar = CreateApplicationMenu();
	RECT  windowRect = { 0, 0,
						 static_cast<LONG>(pApp->GetWidth()),
						 static_cast<LONG>(pApp->GetHeight()) };
	AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, TRUE);

	m_hWnd = CreateWindow(
		windowClass.lpszClassName,
		pApp->GetTitle(),
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr,
		menuBar,
		hInstance,
		pApp);

	pApp->OnInit();

	ShowWindow(m_hWnd, nCmdShow);
	SetForegroundWindow(m_hWnd);
	SetFocus(m_hWnd);

	// Main loop.
	MSG msg = {};
	m_timer.Reset();
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			m_timer.Tick();
			if (!m_appPaused || m_renderRequested)
			{
				pApp->OnUpdate(m_timer);
				pApp->OnRender(m_timer);
				m_renderRequested = false;
			}
			else
			{
				Sleep(100);
			}
		}
	}

	pApp->OnDestroy();
	return static_cast<char>(msg.wParam);
}

void Win32Application::RequestRender()
{
	m_renderRequested = true;
	if (m_hWnd)
	{
		PostMessage(m_hWnd, WM_NULL, 0, 0);
	}
}

LRESULT CALLBACK Win32Application::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	D3D12Application* pApp =
		reinterpret_cast<D3D12Application*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	switch (message)
	{
		// -----------------------------------------------------------------------
		// Window lifecycle
		// -----------------------------------------------------------------------
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
		SetWindowLongPtr(hWnd, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
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

		// -----------------------------------------------------------------------
		// Keyboard
		// -----------------------------------------------------------------------
	case WM_KEYDOWN:
		if (pApp) pApp->OnKeyDown(static_cast<UINT8>(wParam));
		return 0;

	case WM_KEYUP:
		if (pApp) pApp->OnKeyUp(static_cast<UINT8>(wParam));
		return 0;

		// -----------------------------------------------------------------------
		  // Mouse – Left / Middle Button
		  // -----------------------------------------------------------------------
	case WM_LBUTTONDOWN:
		if (pApp)
		{
			SetCapture(hWnd);
			pApp->OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		}
		return 0;

	case WM_LBUTTONUP:
		if (pApp)
		{
			ReleaseCapture();
			pApp->OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		}
		return 0;

	case WM_MBUTTONDOWN:
		if (pApp)
		{
			SetCapture(hWnd);   // keep receiving mouse events even outside window
			pApp->OnMiddleButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		}
		return 0;

	case WM_MBUTTONUP:
		if (pApp)
		{
			ReleaseCapture();
			pApp->OnMiddleButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		}
		return 0;

	case WM_MOUSEMOVE:
		if (pApp)
		{
			pApp->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam);
		}
		return 0;

		// -----------------------------------------------------------------------
		// Mouse – Scroll Wheel (Zoom)
		// -----------------------------------------------------------------------
	case WM_MOUSEWHEEL:
		if (pApp)
		{
			pApp->OnMouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)));
		}
		return 0;

		// -----------------------------------------------------------------------
		// Menu Commands
		// -----------------------------------------------------------------------
	case WM_COMMAND:
	{
		const UINT commandId = LOWORD(wParam);
		if (commandId == IDM_EXIT)
		{
			DestroyWindow(hWnd);
			return 0;
		}
		if (pApp && pApp->OnCommand(commandId))
		{
			return 0;
		}
	}
	break;
	}

	return DefWindowProc(hWnd, message, wParam, lParam);
}