#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "Shared.h"

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

using namespace RenderEngineDetail;

void ZenithRenderEngine::UpdateLightingMenuState() const
{
  // The menus reflect runtime state so the UI always tells the user which
	// optional helpers and lights are currently active.
	HMENU menu = GetMenu(Win32Application::GetHwnd());
	if (!menu)
	{
		return;
	}

	CheckMenuItem(
		menu,
		IDM_VIEW_SOLID_GROUND_PLANE,
		MF_BYCOMMAND | (m_useSolidGroundPlane ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(
		menu,
		IDM_VIEW_ADD_POINT_LIGHT,
		MF_BYCOMMAND | (m_pointLightEnabled ? MF_CHECKED : MF_UNCHECKED));
	ModifyMenuW(
		menu,
		IDM_VIEW_DIRECTIONAL_LIGHT,
		MF_BYCOMMAND | MF_STRING,
		IDM_VIEW_DIRECTIONAL_LIGHT,
		L"Directional Light...");
	ModifyMenuW(
		menu,
		IDM_VIEW_ADD_POINT_LIGHT,
		MF_BYCOMMAND | MF_STRING,
		IDM_VIEW_ADD_POINT_LIGHT,
		m_pointLightEnabled ? L"Remove Point Light" : L"Add Point Light...");
	DrawMenuBar(Win32Application::GetHwnd());
}

void ZenithRenderEngine::EnsureDirectionalLightConfigWindow()
{
	if (m_directionalLightConfigWindow)
	{
		return;
	}

	WNDCLASSEXW windowClass = {};
   // These tool windows are custom Win32 windows rather than dialog resources so
	// their structure stays visible in code for learners.
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = &ZenithRenderEngine::DirectionalLightConfigWindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	windowClass.lpszClassName = DirectionalLightConfigWindowClassName;
	RegisterClassExW(&windowClass);

	RECT mainWindowRect = {};
	GetWindowRect(Win32Application::GetHwnd(), &mainWindowRect);
	m_directionalLightConfigWindow = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		DirectionalLightConfigWindowClassName,
		L"Directional Light",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		mainWindowRect.right + 16,
		mainWindowRect.top + 260,
		320,
		320,
		Win32Application::GetHwnd(),
		nullptr,
		GetModuleHandleW(nullptr),
		this);
}

void ZenithRenderEngine::ShowDirectionalLightConfigWindow()
{
	EnsureDirectionalLightConfigWindow();
	if (!m_directionalLightConfigWindow)
	{
		return;
	}

	SyncDirectionalLightConfigWindow();
	ShowWindow(m_directionalLightConfigWindow, SW_SHOWNORMAL);
	SetWindowPos(m_directionalLightConfigWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(m_directionalLightConfigWindow);
}

void ZenithRenderEngine::SyncDirectionalLightConfigWindow() const
{
    // Synchronization always flows from renderer state -> controls. That makes the
	// sliders/edit boxes a view of the true lighting data, not the other way around.
	if (!m_directionalLightConfigWindow)
	{
		return;
	}

	if (m_directionalLightEnabledCheck)
	{
		Button_SetCheck(m_directionalLightEnabledCheck, m_directionalLightEnabled ? BST_CHECKED : BST_UNCHECKED);
	}
	if (m_directionalLightDirectionXEdit)
	{
		SetWindowTextW(m_directionalLightDirectionXEdit, FormatFloatText(m_directionalLightDirection.x).c_str());
	}
	if (m_directionalLightDirectionYEdit)
	{
		SetWindowTextW(m_directionalLightDirectionYEdit, FormatFloatText(m_directionalLightDirection.y).c_str());
	}
	if (m_directionalLightDirectionZEdit)
	{
		SetWindowTextW(m_directionalLightDirectionZEdit, FormatFloatText(m_directionalLightDirection.z).c_str());
	}
	if (m_directionalLightDirectionXSlider)
	{
		SendMessageW(m_directionalLightDirectionXSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_directionalLightDirection.x, DirectionSliderMin, DirectionSliderMax));
	}
	if (m_directionalLightDirectionYSlider)
	{
		SendMessageW(m_directionalLightDirectionYSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_directionalLightDirection.y, DirectionSliderMin, DirectionSliderMax));
	}
	if (m_directionalLightDirectionZSlider)
	{
		SendMessageW(m_directionalLightDirectionZSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_directionalLightDirection.z, DirectionSliderMin, DirectionSliderMax));
	}
	if (m_directionalLightStrengthEdit)
	{
		SetWindowTextW(m_directionalLightStrengthEdit, FormatFloatText(m_directionalLightStrength).c_str());
	}
	if (m_directionalLightExposureEdit)
	{
		SetWindowTextW(m_directionalLightExposureEdit, FormatFloatText(m_directionalLightExposure).c_str());
	}
	if (m_directionalLightStrengthSlider)
	{
		SendMessageW(m_directionalLightStrengthSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_directionalLightStrength, StrengthSliderMin, StrengthSliderMax));
	}
	if (m_directionalLightExposureSlider)
	{
		SendMessageW(m_directionalLightExposureSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_directionalLightExposure, ExposureSliderMin, ExposureSliderMax));
	}
}

void ZenithRenderEngine::SetDirectionalLightEnabled(bool enabled)
{
	m_directionalLightEnabled = enabled;
	UpdateLightingMenuState();
	SyncDirectionalLightConfigWindow();
	RenderPointLightPreviewFrame();
}

void ZenithRenderEngine::ApplyDirectionalLightConfigFromWindow()
{
	if (!m_directionalLightConfigWindow)
	{
		return;
	}

	float directionX = m_directionalLightDirection.x;
	float directionY = m_directionalLightDirection.y;
	float directionZ = m_directionalLightDirection.z;
	float strength = m_directionalLightStrength;
	float exposure = m_directionalLightExposure;

	if (m_directionalLightDirectionXEdit)
	{
		TryParseFloatFromWindow(m_directionalLightDirectionXEdit, directionX);
	}
	if (m_directionalLightDirectionYEdit)
	{
		TryParseFloatFromWindow(m_directionalLightDirectionYEdit, directionY);
	}
	if (m_directionalLightDirectionZEdit)
	{
		TryParseFloatFromWindow(m_directionalLightDirectionZEdit, directionZ);
	}
	if (m_directionalLightStrengthEdit)
	{
		TryParseFloatFromWindow(m_directionalLightStrengthEdit, strength);
	}
	if (m_directionalLightExposureEdit)
	{
		TryParseFloatFromWindow(m_directionalLightExposureEdit, exposure);
	}

	XMVECTOR direction = XMVectorSet(
      // The UI exposes editable XYZ values, but the runtime still expects a usable
		// non-zero direction vector.
		(std::clamp)(directionX, -1.0f, 1.0f),
		(std::clamp)(directionY, -1.0f, 1.0f),
		(std::clamp)(directionZ, -1.0f, 1.0f),
		0.0f);
	if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-6f)
	{
		direction = XMVectorSet(-0.2f, -1.0f, -0.3f, 0.0f);
	}

	XMStoreFloat3(&m_directionalLightDirection, direction);
	m_directionalLightStrength = (std::max)(0.0f, strength);
	m_directionalLightExposure = exposure;
	if (m_directionalLightEnabledCheck)
	{
		SetDirectionalLightEnabled(Button_GetCheck(m_directionalLightEnabledCheck) == BST_CHECKED);
	}

	SyncDirectionalLightConfigWindow();
	RenderPointLightPreviewFrame();
}

void ZenithRenderEngine::DestroyDirectionalLightConfigWindow()
{
	if (m_directionalLightConfigWindow)
	{
		DestroyWindow(m_directionalLightConfigWindow);
		m_directionalLightConfigWindow = nullptr;
	}

	m_directionalLightEnabledCheck = nullptr;
	m_directionalLightDirectionXEdit = nullptr;
	m_directionalLightDirectionYEdit = nullptr;
	m_directionalLightDirectionZEdit = nullptr;
	m_directionalLightDirectionXSlider = nullptr;
	m_directionalLightDirectionYSlider = nullptr;
	m_directionalLightDirectionZSlider = nullptr;
	m_directionalLightStrengthEdit = nullptr;
	m_directionalLightExposureEdit = nullptr;
	m_directionalLightStrengthSlider = nullptr;
	m_directionalLightExposureSlider = nullptr;
}

void ZenithRenderEngine::SetPointLightEnabled(bool enabled)
{
	if (m_pointLightEnabled == enabled)
	{
		if (enabled)
		{
			ShowPointLightConfigWindow();
		}
		return;
	}

	m_pointLightEnabled = enabled;
    // Enabling the point light is treated as enabling the whole feature: runtime
	// shading, point-shadow generation, gizmo drawing, and config window.
	if (m_pointLightEnabled)
	{
		ShowPointLightConfigWindow();
	}
	else
	{
		m_isPointLightDragging = false;
		if (m_isPointLightHovered)
		{
			m_isPointLightHovered = false;
			UpdatePointLightGizmoVertexBuffer();
		}

		if (m_pointLightConfigWindow)
		{
			ShowWindow(m_pointLightConfigWindow, SW_HIDE);
		}
	}

	UpdateLightingMenuState();
	RenderPointLightPreviewFrame();
}

void ZenithRenderEngine::EnsurePointLightConfigWindow()
{
	if (m_pointLightConfigWindow)
	{
		return;
	}

	WNDCLASSEXW windowClass = {};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = &ZenithRenderEngine::PointLightConfigWindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	windowClass.lpszClassName = PointLightConfigWindowClassName;
	RegisterClassExW(&windowClass);

	RECT mainWindowRect = {};
	GetWindowRect(Win32Application::GetHwnd(), &mainWindowRect);
	m_pointLightConfigWindow = CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
		PointLightConfigWindowClassName,
		L"Point Light",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		mainWindowRect.right + 16,
		mainWindowRect.top,
		280,
		240,
		Win32Application::GetHwnd(),
		nullptr,
		GetModuleHandleW(nullptr),
		this);
}

void ZenithRenderEngine::ShowPointLightConfigWindow()
{
	EnsurePointLightConfigWindow();
	if (!m_pointLightConfigWindow)
	{
		return;
	}

	SyncPointLightConfigWindow();
	ShowWindow(m_pointLightConfigWindow, SW_SHOWNORMAL);
	SetWindowPos(m_pointLightConfigWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(m_pointLightConfigWindow);
}

void ZenithRenderEngine::SyncPointLightConfigWindow() const
{
	if (!m_pointLightConfigWindow)
	{
		return;
	}

	if (m_pointLightStrengthEdit)
	{
		SetWindowTextW(m_pointLightStrengthEdit, FormatFloatText(m_pointLightStrength).c_str());
	}
	if (m_pointLightExposureEdit)
	{
		SetWindowTextW(m_pointLightExposureEdit, FormatFloatText(m_pointLightExposure).c_str());
	}
	if (m_pointLightRangeEdit)
	{
		SetWindowTextW(m_pointLightRangeEdit, FormatFloatText(m_pointLightRange).c_str());
	}
	if (m_pointLightStrengthSlider)
	{
		SendMessageW(m_pointLightStrengthSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_pointLightStrength, StrengthSliderMin, StrengthSliderMax));
	}
	if (m_pointLightExposureSlider)
	{
		SendMessageW(m_pointLightExposureSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_pointLightExposure, ExposureSliderMin, ExposureSliderMax));
	}
	if (m_pointLightRangeSlider)
	{
		SendMessageW(m_pointLightRangeSlider, TBM_SETPOS, TRUE, FloatToSliderPosition(m_pointLightRange, RangeSliderMin, RangeSliderMax));
	}
	if (m_pointLightColorText)
	{
		wchar_t colorText[128] = {};
		swprintf_s(
			colorText,
			L"Color: %.2f, %.2f, %.2f",
			m_pointLightColor.x,
			m_pointLightColor.y,
			m_pointLightColor.z);
		SetWindowTextW(m_pointLightColorText, colorText);
	}
}

void ZenithRenderEngine::ApplyPointLightConfigFromWindow()
{
	if (!m_pointLightConfigWindow)
	{
		return;
	}

	float strength = m_pointLightStrength;
	float exposure = m_pointLightExposure;
	float range = m_pointLightRange;

	if (m_pointLightStrengthEdit)
	{
		TryParseFloatFromWindow(m_pointLightStrengthEdit, strength);
	}
	if (m_pointLightExposureEdit)
	{
		TryParseFloatFromWindow(m_pointLightExposureEdit, exposure);
	}
	if (m_pointLightRangeEdit)
	{
		TryParseFloatFromWindow(m_pointLightRangeEdit, range);
	}

	m_pointLightStrength = (std::max)(0.0f, strength);
	m_pointLightExposure = exposure;
	m_pointLightRange = (std::max)(0.1f, range);
	SyncPointLightConfigWindow();
	RenderPointLightPreviewFrame();
}

void ZenithRenderEngine::ChoosePointLightColor()
{
  // The Win32 common color picker returns 8-bit RGB values, which are then
	// converted back into the renderer's normalized 0..1 float representation.
	CHOOSECOLORW chooseColor = {};
	COLORREF customColors[16] = {};
	COLORREF initialColor = FloatColorToColorRef(m_pointLightColor);
	chooseColor.lStructSize = sizeof(chooseColor);
	chooseColor.hwndOwner = m_pointLightConfigWindow;
	chooseColor.rgbResult = initialColor;
	chooseColor.lpCustColors = customColors;
	chooseColor.Flags = CC_FULLOPEN | CC_RGBINIT;

	if (!ChooseColorW(&chooseColor))
	{
		return;
	}

	m_pointLightColor = ColorRefToFloatColor(chooseColor.rgbResult);
	UpdatePointLightGizmoVertexBuffer();
	SyncPointLightConfigWindow();
	RenderPointLightPreviewFrame();
}

void ZenithRenderEngine::DestroyPointLightConfigWindow()
{
	if (m_pointLightConfigWindow)
	{
		DestroyWindow(m_pointLightConfigWindow);
		m_pointLightConfigWindow = nullptr;
	}

	m_pointLightStrengthEdit = nullptr;
	m_pointLightExposureEdit = nullptr;
	m_pointLightRangeEdit = nullptr;
	m_pointLightStrengthSlider = nullptr;
	m_pointLightExposureSlider = nullptr;
	m_pointLightRangeSlider = nullptr;
	m_pointLightColorText = nullptr;
}

void ZenithRenderEngine::RenderPointLightPreviewFrame()
{
	Win32Application::RequestRender();
}

LRESULT CALLBACK ZenithRenderEngine::DirectionalLightConfigWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	ZenithRenderEngine* engine = reinterpret_cast<ZenithRenderEngine*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	switch (message)
	{
	case WM_NCCREATE:
	{
		const CREATESTRUCTW* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
		return TRUE;
	}
	case WM_CREATE:
	{
		engine = reinterpret_cast<ZenithRenderEngine*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (!engine)
		{
			return -1;
		}

		HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      // The window is composed manually from standard Win32 controls so readers can
		// see exactly how sliders, edits, and buttons map to renderer state.
		engine->m_directionalLightEnabledCheck = CreateWindowW(L"BUTTON", L"Enabled", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, 12, 90, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightEnabledCheckId)), nullptr, nullptr);
		CreateWindowW(L"STATIC", L"Direction X", WS_CHILD | WS_VISIBLE, 12, 46, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_directionalLightDirectionXSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 42, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionXSliderId)), nullptr, nullptr);
		engine->m_directionalLightDirectionXEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 228, 44, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionXEditId)), nullptr, nullptr);
		SendMessageW(engine->m_directionalLightDirectionXSlider, TBM_SETRANGEMIN, FALSE, DirectionSliderMin);
		SendMessageW(engine->m_directionalLightDirectionXSlider, TBM_SETRANGEMAX, FALSE, DirectionSliderMax);
		CreateWindowW(L"STATIC", L"Direction Y", WS_CHILD | WS_VISIBLE, 12, 78, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_directionalLightDirectionYSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 74, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionYSliderId)), nullptr, nullptr);
		engine->m_directionalLightDirectionYEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 228, 76, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionYEditId)), nullptr, nullptr);
		SendMessageW(engine->m_directionalLightDirectionYSlider, TBM_SETRANGEMIN, FALSE, DirectionSliderMin);
		SendMessageW(engine->m_directionalLightDirectionYSlider, TBM_SETRANGEMAX, FALSE, DirectionSliderMax);
		CreateWindowW(L"STATIC", L"Direction Z", WS_CHILD | WS_VISIBLE, 12, 110, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_directionalLightDirectionZSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 106, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionZSliderId)), nullptr, nullptr);
		engine->m_directionalLightDirectionZEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 228, 108, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightDirectionZEditId)), nullptr, nullptr);
		SendMessageW(engine->m_directionalLightDirectionZSlider, TBM_SETRANGEMIN, FALSE, DirectionSliderMin);
		SendMessageW(engine->m_directionalLightDirectionZSlider, TBM_SETRANGEMAX, FALSE, DirectionSliderMax);
		CreateWindowW(L"STATIC", L"Strength", WS_CHILD | WS_VISIBLE, 12, 142, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_directionalLightStrengthSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 138, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightStrengthSliderId)), nullptr, nullptr);
		engine->m_directionalLightStrengthEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 228, 140, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightStrengthEditId)), nullptr, nullptr);
		SendMessageW(engine->m_directionalLightStrengthSlider, TBM_SETRANGEMIN, FALSE, StrengthSliderMin);
		SendMessageW(engine->m_directionalLightStrengthSlider, TBM_SETRANGEMAX, FALSE, StrengthSliderMax);
		CreateWindowW(L"STATIC", L"Exposure", WS_CHILD | WS_VISIBLE, 12, 178, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_directionalLightExposureSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 174, 120, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightExposureSliderId)), nullptr, nullptr);
		engine->m_directionalLightExposureEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 228, 176, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightExposureEditId)), nullptr, nullptr);
		SendMessageW(engine->m_directionalLightExposureSlider, TBM_SETRANGEMIN, FALSE, ExposureSliderMin);
		SendMessageW(engine->m_directionalLightExposureSlider, TBM_SETRANGEMAX, FALSE, ExposureSliderMax);
		HWND applyButton = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 170, 246, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(DirectionalLightApplyButtonId)), nullptr, nullptr);

		const HWND controls[] = {
			engine->m_directionalLightEnabledCheck,
			engine->m_directionalLightDirectionXEdit,
			engine->m_directionalLightDirectionYEdit,
			engine->m_directionalLightDirectionZEdit,
			engine->m_directionalLightDirectionXSlider,
			engine->m_directionalLightDirectionYSlider,
			engine->m_directionalLightDirectionZSlider,
			engine->m_directionalLightStrengthEdit,
			engine->m_directionalLightExposureEdit,
			engine->m_directionalLightStrengthSlider,
			engine->m_directionalLightExposureSlider,
			applyButton
		};
		for (HWND control : controls)
		{
			SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}

		engine->SyncDirectionalLightConfigWindow();
		return 0;
	}
	case WM_COMMAND:
		if (!engine)
		{
			break;
		}

		switch (LOWORD(wParam))
		{
		case DirectionalLightEnabledCheckId:
			if (HIWORD(wParam) == BN_CLICKED)
			{
				engine->SetDirectionalLightEnabled(Button_GetCheck(engine->m_directionalLightEnabledCheck) == BST_CHECKED);
				return 0;
			}
			break;
		case DirectionalLightApplyButtonId:
			engine->ApplyDirectionalLightConfigFromWindow();
			return 0;
		default:
			break;
		}
		break;
	case WM_HSCROLL:
		if (!engine)
		{
			break;
		}

     // Slider changes update renderer values immediately so the user gets live
		// visual feedback without pressing Apply.
		if (reinterpret_cast<HWND>(lParam) == engine->m_directionalLightStrengthSlider)
		{
			engine->m_directionalLightStrength = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_directionalLightStrengthSlider, TBM_GETPOS, 0, 0)));
			engine->SyncDirectionalLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_directionalLightDirectionXSlider)
		{
			engine->m_directionalLightDirection.x = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_directionalLightDirectionXSlider, TBM_GETPOS, 0, 0)));
			engine->SyncDirectionalLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_directionalLightDirectionYSlider)
		{
			engine->m_directionalLightDirection.y = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_directionalLightDirectionYSlider, TBM_GETPOS, 0, 0)));
			engine->SyncDirectionalLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_directionalLightDirectionZSlider)
		{
			engine->m_directionalLightDirection.z = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_directionalLightDirectionZSlider, TBM_GETPOS, 0, 0)));
			engine->SyncDirectionalLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_directionalLightExposureSlider)
		{
			engine->m_directionalLightExposure = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_directionalLightExposureSlider, TBM_GETPOS, 0, 0)));
			engine->SyncDirectionalLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		break;
	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		if (engine)
		{
			engine->m_directionalLightConfigWindow = nullptr;
			engine->m_directionalLightEnabledCheck = nullptr;
			engine->m_directionalLightDirectionXEdit = nullptr;
			engine->m_directionalLightDirectionYEdit = nullptr;
			engine->m_directionalLightDirectionZEdit = nullptr;
			engine->m_directionalLightDirectionXSlider = nullptr;
			engine->m_directionalLightDirectionYSlider = nullptr;
			engine->m_directionalLightDirectionZSlider = nullptr;
			engine->m_directionalLightStrengthEdit = nullptr;
			engine->m_directionalLightExposureEdit = nullptr;
			engine->m_directionalLightStrengthSlider = nullptr;
			engine->m_directionalLightExposureSlider = nullptr;
		}
		return 0;
	default:
		break;
	}

	return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ZenithRenderEngine::PointLightConfigWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	ZenithRenderEngine* engine = reinterpret_cast<ZenithRenderEngine*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

	switch (message)
	{
	case WM_NCCREATE:
	{
		const CREATESTRUCTW* createStruct = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
		return TRUE;
	}
	case WM_CREATE:
	{
		engine = reinterpret_cast<ZenithRenderEngine*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (!engine)
		{
			return -1;
		}

		HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      // The point-light window mirrors the directional-light layout on purpose so
		// both editing experiences teach the same UI pattern.
		CreateWindowW(L"STATIC", L"Strength", WS_CHILD | WS_VISIBLE, 12, 14, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_pointLightStrengthSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 10, 92, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightStrengthSliderId)), nullptr, nullptr);
		engine->m_pointLightStrengthEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 198, 12, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightStrengthEditId)), nullptr, nullptr);
		SendMessageW(engine->m_pointLightStrengthSlider, TBM_SETRANGEMIN, FALSE, StrengthSliderMin);
		SendMessageW(engine->m_pointLightStrengthSlider, TBM_SETRANGEMAX, FALSE, StrengthSliderMax);
		CreateWindowW(L"STATIC", L"Exposure", WS_CHILD | WS_VISIBLE, 12, 52, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_pointLightExposureSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 48, 92, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightExposureSliderId)), nullptr, nullptr);
		engine->m_pointLightExposureEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 198, 50, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightExposureEditId)), nullptr, nullptr);
		SendMessageW(engine->m_pointLightExposureSlider, TBM_SETRANGEMIN, FALSE, ExposureSliderMin);
		SendMessageW(engine->m_pointLightExposureSlider, TBM_SETRANGEMAX, FALSE, ExposureSliderMax);
		CreateWindowW(L"STATIC", L"Range", WS_CHILD | WS_VISIBLE, 12, 90, 80, 20, hwnd, nullptr, nullptr, nullptr);
		engine->m_pointLightRangeSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 100, 86, 92, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightRangeSliderId)), nullptr, nullptr);
		engine->m_pointLightRangeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 198, 88, 52, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightRangeEditId)), nullptr, nullptr);
		SendMessageW(engine->m_pointLightRangeSlider, TBM_SETRANGEMIN, FALSE, RangeSliderMin);
		SendMessageW(engine->m_pointLightRangeSlider, TBM_SETRANGEMAX, FALSE, RangeSliderMax);
		engine->m_pointLightColorText = CreateWindowW(L"STATIC", L"Color", WS_CHILD | WS_VISIBLE, 12, 118, 238, 20, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightColorTextId)), nullptr, nullptr);
		HWND colorButton = CreateWindowW(L"BUTTON", L"Choose Color...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 146, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightColorButtonId)), nullptr, nullptr);
		HWND applyButton = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 140, 146, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(PointLightApplyButtonId)), nullptr, nullptr);

		const HWND controls[] = {
			engine->m_pointLightStrengthEdit,
			engine->m_pointLightExposureEdit,
			engine->m_pointLightRangeEdit,
			engine->m_pointLightStrengthSlider,
			engine->m_pointLightExposureSlider,
			engine->m_pointLightRangeSlider,
			engine->m_pointLightColorText,
			colorButton,
			applyButton
		};
		for (HWND control : controls)
		{
			SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}

		engine->SyncPointLightConfigWindow();
		return 0;
	}
	case WM_COMMAND:
		if (!engine)
		{
			break;
		}

		switch (LOWORD(wParam))
		{
		case PointLightApplyButtonId:
			engine->ApplyPointLightConfigFromWindow();
			return 0;
		case PointLightColorButtonId:
			engine->ChoosePointLightColor();
			return 0;
		default:
			break;
		}
		break;
	case WM_HSCROLL:
		if (!engine)
		{
			break;
		}

		if (reinterpret_cast<HWND>(lParam) == engine->m_pointLightStrengthSlider)
		{
			engine->m_pointLightStrength = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_pointLightStrengthSlider, TBM_GETPOS, 0, 0)));
			engine->SyncPointLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_pointLightExposureSlider)
		{
			engine->m_pointLightExposure = SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_pointLightExposureSlider, TBM_GETPOS, 0, 0)));
			engine->SyncPointLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		if (reinterpret_cast<HWND>(lParam) == engine->m_pointLightRangeSlider)
		{
			engine->m_pointLightRange = (std::max)(0.1f, SliderPositionToFloat(static_cast<int>(SendMessageW(engine->m_pointLightRangeSlider, TBM_GETPOS, 0, 0))));
			engine->SyncPointLightConfigWindow();
			engine->RenderPointLightPreviewFrame();
			return 0;
		}
		break;
	case WM_CLOSE:
		ShowWindow(hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		if (engine)
		{
			engine->m_pointLightConfigWindow = nullptr;
			engine->m_pointLightStrengthEdit = nullptr;
			engine->m_pointLightExposureEdit = nullptr;
			engine->m_pointLightRangeEdit = nullptr;
			engine->m_pointLightStrengthSlider = nullptr;
			engine->m_pointLightExposureSlider = nullptr;
			engine->m_pointLightRangeSlider = nullptr;
			engine->m_pointLightColorText = nullptr;
		}
		return 0;
	default:
		break;
	}

	return DefWindowProcW(hwnd, message, wParam, lParam);
}
