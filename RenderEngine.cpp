#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"
#include "DescriptorAllocator.h"
#include <CommCtrl.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

// ---------------------------------------------------------------------------
// ZenithRenderEngine
//
// This file is the "high-level renderer" for the sample. It sits above the
// lower-level D3D12RenderContext and is responsible for:
//   1. creating the root signature and pipeline states
//   2. creating per-frame constant buffers
//   3. loading a model and framing the camera around it
//   4. updating scene / lighting data every frame
//   5. binding descriptors and issuing draw calls
//
// A useful mental model is:
//   - D3D12RenderContext = platform / GPU plumbing
//   - ZenithRenderEngine = actual scene rendering policy
//
// Shadow features added so far in this file:
//   1. create depth-only PSOs for rendering from the light
//   2. build a light view/projection every frame
//   3. fit the orthographic shadow frustum to the model bounds
//   4. render a shadow pass before the main pass
//   5. provide an optional solid ground plane to inspect shadow quality
//
// So when reading this file from top to bottom, the beginner flow is:
//   - setup GPU states and buffers in `OnInit()`
//   - compute camera + light transforms in `OnUpdate()`
//   - render shadow map first in `OnRender()`
//   - render the normal shaded scene second in `OnRender()`
// ---------------------------------------------------------------------------

namespace
{
	constexpr wchar_t DirectionalLightConfigWindowClassName[] = L"ZenithDirectionalLightConfigWindow";
	// Control IDs for the tiny lighting tool windows.
	// They are grouped here so the dialog procedures can switch on them cleanly.
	constexpr int DirectionalLightEnabledCheckId = 1101;
	constexpr int DirectionalLightDirectionXEditId = 1102;
	constexpr int DirectionalLightDirectionYEditId = 1103;
	constexpr int DirectionalLightDirectionZEditId = 1104;
	constexpr int DirectionalLightDirectionXSliderId = 1110;
	constexpr int DirectionalLightDirectionYSliderId = 1111;
	constexpr int DirectionalLightDirectionZSliderId = 1112;
	constexpr int DirectionalLightStrengthEditId = 1105;
	constexpr int DirectionalLightExposureEditId = 1106;
	constexpr int DirectionalLightApplyButtonId = 1107;
	constexpr int DirectionalLightStrengthSliderId = 1108;
	constexpr int DirectionalLightExposureSliderId = 1109;
	constexpr wchar_t PointLightConfigWindowClassName[] = L"ZenithPointLightConfigWindow";
	constexpr int PointLightStrengthEditId = 1001;
	constexpr int PointLightExposureEditId = 1002;
	constexpr int PointLightRangeEditId = 1003;
	constexpr int PointLightColorButtonId = 1004;
	constexpr int PointLightApplyButtonId = 1005;
	constexpr int PointLightColorTextId = 1006;
	constexpr int PointLightStrengthSliderId = 1007;
	constexpr int PointLightExposureSliderId = 1008;
	constexpr int PointLightRangeSliderId = 1009;
	// Sliders operate on integers, but the renderer stores light values as floats.
	// `LightSliderScale = 10` means:
	//   slider value 15 -> float 1.5
	//   slider value -8 -> float -0.8
	// This gives a simple one-decimal-place UI without needing custom controls.
	constexpr int LightSliderScale = 10;
	constexpr int DirectionSliderMin = -10;
	constexpr int DirectionSliderMax = 10;
	constexpr int StrengthSliderMin = 0;
	constexpr int StrengthSliderMax = 2000;
	constexpr int ExposureSliderMin = -80;
	constexpr int ExposureSliderMax = 80;
	constexpr int RangeSliderMin = 1;
	constexpr int RangeSliderMax = 1000;

	struct MouseRay
	{
		XMVECTOR origin;
		XMVECTOR direction;
	};

	// Helper used when the Win32 file dialog returns a UTF-16 path but the
	 // model loader expects UTF-8. The conversion itself is not D3D12-specific;
	 // it is just glue between Win32 and the asset-loading layer.
	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
		{
			return {};
		}

		const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (requiredSize <= 1)
		{
			return {};
		}

		std::vector<char> buffer(static_cast<size_t>(requiredSize));
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), requiredSize, nullptr, nullptr);

		return std::string(buffer.data());
	}

	bool TryBuildMouseRay(int x, int y, float viewportWidth, float viewportHeight, FXMMATRIX view, CXMMATRIX projection, MouseRay& ray)
	{
		if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
		{
			return false;
		}

		const XMMATRIX identity = XMMatrixIdentity();
		const XMVECTOR nearPoint = XMVector3Unproject(
			XMVectorSet(static_cast<float>(x), static_cast<float>(y), 0.0f, 1.0f),
			0.0f,
			0.0f,
			viewportWidth,
			viewportHeight,
			0.0f,
			1.0f,
			projection,
			view,
			identity);
		const XMVECTOR farPoint = XMVector3Unproject(
			XMVectorSet(static_cast<float>(x), static_cast<float>(y), 1.0f, 1.0f),
			0.0f,
			0.0f,
			viewportWidth,
			viewportHeight,
			0.0f,
			1.0f,
			projection,
			view,
			identity);

		ray.origin = nearPoint;
		ray.direction = XMVector3Normalize(farPoint - nearPoint);
		return true;
	}

	bool TryIntersectPlane(FXMVECTOR rayOrigin, FXMVECTOR rayDirection, FXMVECTOR planePoint, FXMVECTOR planeNormal, XMVECTOR& hitPoint)
	{
		const float denominator = XMVectorGetX(XMVector3Dot(rayDirection, planeNormal));
		if (fabsf(denominator) < 1e-5f)
		{
			return false;
		}

		const XMVECTOR originToPlane = planePoint - rayOrigin;
		const float t = XMVectorGetX(XMVector3Dot(originToPlane, planeNormal)) / denominator;
		if (t < 0.0f)
		{
			return false;
		}

		hitPoint = rayOrigin + rayDirection * t;
		return true;
	}

	bool TryProjectWorldToScreen(FXMVECTOR worldPosition, float viewportWidth, float viewportHeight, FXMMATRIX view, CXMMATRIX projection, XMFLOAT2& screenPosition)
	{
		if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
		{
			return false;
		}

		const XMVECTOR projected = XMVector3Project(
			worldPosition,
			0.0f,
			0.0f,
			viewportWidth,
			viewportHeight,
			0.0f,
			1.0f,
			projection,
			view,
			XMMatrixIdentity());

		const float projectedDepth = XMVectorGetZ(projected);
		if (projectedDepth < 0.0f || projectedDepth > 1.0f)
		{
			return false;
		}

		screenPosition.x = XMVectorGetX(projected);
		screenPosition.y = XMVectorGetY(projected);
		return true;
	}

	bool IsScreenPointNear(float x, float y, const XMFLOAT2& target, float radiusPixels)
	{
		const float dx = x - target.x;
		const float dy = y - target.y;
		return (dx * dx + dy * dy) <= radiusPixels * radiusPixels;
	}

	bool TryComputePointLightScreenBounds(
		const XMFLOAT3& pointLightPosition,
		float pointLightGizmoScale,
		float viewportWidth,
		float viewportHeight,
		FXMMATRIX view,
		CXMMATRIX projection,
		RECT& screenBounds)
	{
		if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
		{
			return false;
		}

		const XMFLOAT3 cubeCorners[] = {
			XMFLOAT3(-1.0f, -1.0f, -1.0f),
			XMFLOAT3(1.0f, -1.0f, -1.0f),
			XMFLOAT3(-1.0f,  1.0f, -1.0f),
			XMFLOAT3(1.0f,  1.0f, -1.0f),
			XMFLOAT3(-1.0f, -1.0f,  1.0f),
			XMFLOAT3(1.0f, -1.0f,  1.0f),
			XMFLOAT3(-1.0f,  1.0f,  1.0f),
			XMFLOAT3(1.0f,  1.0f,  1.0f)
		};

		float minX = FLT_MAX;
		float minY = FLT_MAX;
		float maxX = -FLT_MAX;
		float maxY = -FLT_MAX;
		bool projectedAnyCorner = false;

		for (const XMFLOAT3& corner : cubeCorners)
		{
			const XMVECTOR worldCorner = XMVectorSet(
				pointLightPosition.x + corner.x * pointLightGizmoScale,
				pointLightPosition.y + corner.y * pointLightGizmoScale,
				pointLightPosition.z + corner.z * pointLightGizmoScale,
				1.0f);

			XMFLOAT2 projectedCorner;
			if (!TryProjectWorldToScreen(worldCorner, viewportWidth, viewportHeight, view, projection, projectedCorner))
			{
				continue;
			}

			projectedAnyCorner = true;
			minX = (std::min)(minX, projectedCorner.x);
			minY = (std::min)(minY, projectedCorner.y);
			maxX = (std::max)(maxX, projectedCorner.x);
			maxY = (std::max)(maxY, projectedCorner.y);
		}

		if (!projectedAnyCorner)
		{
			return false;
		}

		screenBounds.left = static_cast<LONG>(floorf(minX));
		screenBounds.top = static_cast<LONG>(floorf(minY));
		screenBounds.right = static_cast<LONG>(ceilf(maxX));
		screenBounds.bottom = static_cast<LONG>(ceilf(maxY));
		return true;
	}

	bool IsScreenPointInsideBounds(float x, float y, const RECT& bounds, float paddingPixels)
	{
		return x >= static_cast<float>(bounds.left) - paddingPixels &&
			x <= static_cast<float>(bounds.right) + paddingPixels &&
			y >= static_cast<float>(bounds.top) - paddingPixels &&
			y <= static_cast<float>(bounds.bottom) + paddingPixels;
	}

	std::wstring FormatFloatText(float value)
	{
		wchar_t buffer[64] = {};
		swprintf_s(buffer, L"%.2f", value);
		return std::wstring(buffer);
	}

	bool TryParseFloatFromWindow(HWND window, float& value)
	{
		wchar_t buffer[64] = {};
		GetWindowTextW(window, buffer, _countof(buffer));
		wchar_t* end = nullptr;
		const float parsedValue = wcstof(buffer, &end);
		if (end == buffer)
		{
			return false;
		}

		value = parsedValue;
		return true;
	}

	XMFLOAT3 MakePointLightHoverColor(const XMFLOAT3& baseColor)
	{
		return XMFLOAT3(
			(std::min)(1.0f, baseColor.x * 0.65f + 0.35f),
			(std::min)(1.0f, baseColor.y * 0.65f + 0.35f),
			(std::min)(1.0f, baseColor.z * 0.65f + 0.35f));
	}

	COLORREF FloatColorToColorRef(const XMFLOAT3& color)
	{
		const BYTE red = static_cast<BYTE>((std::clamp)(color.x, 0.0f, 1.0f) * 255.0f);
		const BYTE green = static_cast<BYTE>((std::clamp)(color.y, 0.0f, 1.0f) * 255.0f);
		const BYTE blue = static_cast<BYTE>((std::clamp)(color.z, 0.0f, 1.0f) * 255.0f);
		return RGB(red, green, blue);
	}

	XMFLOAT3 ColorRefToFloatColor(COLORREF color)
	{
		return XMFLOAT3(
			static_cast<float>(GetRValue(color)) / 255.0f,
			static_cast<float>(GetGValue(color)) / 255.0f,
			static_cast<float>(GetBValue(color)) / 255.0f);
	}

	int FloatToSliderPosition(float value, int sliderMin, int sliderMax)
	{
		// Convert a float parameter into an integer trackbar position while keeping
		// the value inside the slider's legal range.
		const int scaledValue = static_cast<int>(lroundf(value * static_cast<float>(LightSliderScale)));
		return (std::clamp)(scaledValue, sliderMin, sliderMax);
	}

	float SliderPositionToFloat(int sliderPosition)
	{
		// Reverse of `FloatToSliderPosition()`: map the integer thumb position back
		 // into the float value used by lighting calculations.
		return static_cast<float>(sliderPosition) / static_cast<float>(LightSliderScale);
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ZenithRenderEngine::ZenithRenderEngine(UINT width, UINT height, std::wstring name)
	: D3D12Application(width, height, name)
	, m_model(nullptr)
	, m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height))
	, m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
	, m_sceneDataCbData{}
	, m_pSceneDataCbvDataBegin(nullptr)
	, m_lightingDataCbData{}
	, m_pLightingDataCbvDataBegin(nullptr)
{
	// Configure the camera lens once we know the viewport size
	m_camera.SetLens(XM_PIDIV4,
		static_cast<float>(width) / static_cast<float>(height),
		0.1f, 500.0f);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ZenithRenderEngine::OnInit()
{
	// Trackbars live in the common-controls library instead of the basic USER32
	 // control set, so they must be initialized before any slider windows are made.
	INITCOMMONCONTROLSEX commonControls = {};
	commonControls.dwSize = sizeof(commonControls);
	commonControls.dwICC = ICC_BAR_CLASSES;
	InitCommonControlsEx(&commonControls);

	// 1. Initialize D3D12 Context (Device, Swap Chain, Command Queue, RTV Heap...)
	m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

	// 2. Create all long-lived rendering objects.
		 //
		 // These are the "engine state" objects that usually live for most of the
		// program: root signature, PSOs, persistent upload buffers, and the grid.
		 //
		 // For the shadow work added so far, this stage creates:
		 //   - main scene PSOs
		 //   - shadow-only PSOs
		 //   - constant buffers for the camera pass, light pass, and inspection plane
		 //   - helper geometry like the line grid and optional solid ground plane
	CreateRootSignature();
	// The renderer now creates four model PSOs so meshes can be dispatched by the
	  // combination of transparency and double-sided state.
	CreatePipelineState();
	CreateDoubleSidedPipelineState();
	CreateTransparentPipelineState();
	CreateDoubleSidedTransparentPipelineState();
	CreateGridPipelineState();
	CreatePointLightGizmoPipelineState();
	CreateSceneDataConstantBuffer();
	CreatePointLightSceneDataConstantBuffer();
	// The shadow pass needs its own SceneData buffer because its camera is the light,
	  // not the user's view camera.
	CreateShadowSceneDataConstantBuffer();
	CreatePointShadowSceneDataConstantBuffers();
	// The optional inspection plane also gets its own SceneData because it does not
	   // use the model's translated world transform.
	CreateGroundPlaneSceneDataConstantBuffer();
	CreateLightingDataConstantBuffer();
	// Shadow PSOs are depth-only render states used before the main color pass.
	CreateShadowPipelineState();
	CreateDoubleSidedShadowPipelineState();
	CreateGridVertexBuffer();
	CreatePointLightGizmoVertexBuffer();
	// This plane is only for debugging / viewing shadows better.
	CreateGroundPlaneVertexBuffer();
	CreateGroundPlaneMaterialConstantBuffer();
	UpdateLightingMenuState();
}

bool ZenithRenderEngine::OnCommand(UINT commandId)
{
	switch (commandId)
	{
	case IDM_FILE_LOAD_MODEL:
	{
		wchar_t filePath[MAX_PATH] = {};
		OPENFILENAMEW openFileName = {};
		openFileName.lStructSize = sizeof(openFileName);
		openFileName.hwndOwner = Win32Application::GetHwnd();
		openFileName.lpstrFile = filePath;
		openFileName.nMaxFile = _countof(filePath);
		openFileName.lpstrFilter = L"3D Model Files\0*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.stl;*.ply\0All Files\0*.*\0\0";
		openFileName.nFilterIndex = 1;
		openFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

		if (GetOpenFileNameW(&openFileName))
		{
			LoadModelFromPath(filePath);
		}

		return true;
	}
	case IDM_ABOUT:
		MessageBoxW(
			Win32Application::GetHwnd(),
			L"Zenith\nDirect3D 12 model viewer\n\nUse Files > Load Model to open a supported 3D asset.",
			GetTitle(),
			MB_OK | MB_ICONINFORMATION);
		return true;
	case IDM_RENDER_IMAGE:
	{
		wchar_t filePath[MAX_PATH] = L"render.png";
		OPENFILENAMEW saveFileName = {};
		saveFileName.lStructSize = sizeof(saveFileName);
		saveFileName.hwndOwner = Win32Application::GetHwnd();
		saveFileName.lpstrFile = filePath;
		saveFileName.nMaxFile = _countof(filePath);
		saveFileName.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0\0";
		saveFileName.nFilterIndex = 1;
		saveFileName.lpstrDefExt = L"png";
		saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

		if (GetSaveFileNameW(&saveFileName))
		{
			m_pendingRenderImagePath = filePath;
		}

		return true;
	}
	case IDM_VIEW_DIRECTIONAL_LIGHT:
		ShowDirectionalLightConfigWindow();
		return true;
	case IDM_VIEW_SOLID_GROUND_PLANE:
		// Swap the visual helper from a wire grid to a solid plane.
		   // The plane makes it much easier to judge whether shadows look correct.
		m_useSolidGroundPlane = !m_useSolidGroundPlane;
		UpdateLightingMenuState();
		return true;
	case IDM_VIEW_ADD_POINT_LIGHT:
		SetPointLightEnabled(!m_pointLightEnabled);
		return true;
	default:
		return false;
	}
}

void ZenithRenderEngine::UpdateLightingMenuState() const
{
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
	if (!m_directionalLightConfigWindow)
	{
		return;
	}

	// "Sync" means pushing the renderer's current runtime values back into the UI.
	   // This is called both when the window opens and after programmatic changes so
	   // the sliders, check box, and edit boxes always agree with each other.
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

	// Read exact values from the edit controls. Sliders already update the runtime
	 // state live, but the Apply button is still useful when a learner types values
	 // manually and wants those edits validated and committed in one step.
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

	// Direction sliders and text fields operate in the intuitive -1..1 range.
	  // The vector is clamped to that range, then a safe fallback is used if the
	  // user accidentally enters (0, 0, 0), because a zero light direction cannot
	  // be normalized into a valid lighting direction.
	XMVECTOR direction = XMVectorSet(
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
	// The point light is treated more like a scene object than a simple checkbox:
	   // enabling it shows the tool window and the viewport gizmo, while disabling it
	   // hides the window and cancels any drag in progress.
	if (m_pointLightEnabled == enabled)
	{
		if (enabled)
		{
			ShowPointLightConfigWindow();
		}
		return;
	}

	m_pointLightEnabled = enabled;
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

void ZenithRenderEngine::CreateRootSignature()
{
	auto device = m_renderContext->GetDevice();

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

	// This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
	{
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	// Root-signature layout overview:
	//   Slot 0: SRV descriptor table for model textures (t0...)
	//   Slot 1: CBV descriptor table for SceneData (b0)
	//   Slot 2: Root CBV for per-mesh MaterialData (b1)
	//   Slot 3: CBV descriptor table for LightingData (b2)
	//
	// The main rule being used here is:
	//   - heap-managed resources -> descriptor tables
	//   - tiny per-draw/per-mesh buffer addresses -> root CBV is still fine
	CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
	ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

	// 1. Tạo Root Signature
	CD3DX12_ROOT_PARAMETER1 rootParameters[4];
	// Root param 0: SRV heap (texture array) → pixel shader
	rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

	// Root param 1: SceneData b0 → dynamic CBV descriptor
	rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_VERTEX);

	// Root param 2: MaterialData b1 → pixel shader
	rootParameters[2].InitAsConstantBufferView(1, 0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_PIXEL);

	// Root param 3: LightingData b2 → dynamic CBV descriptor
	rootParameters[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);

	// Static samplers are baked into the root signature instead of stored in a
	   // descriptor heap. This keeps sampler management simple for a beginner sample.
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_ANISOTROPIC;
	sampler.MaxAnisotropy = 8;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, rootSignatureFlags);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
	ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

void ZenithRenderEngine::CreatePipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	// Load file shaders.hlsl (bạn cần tạo file này trong project)
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	// This must exactly match the `Vertex` struct in `Mesh.h`.
	//
	// Offsets:
	//   0  -> position
	//   12 -> normal
	//   24 -> uv
	//   32 -> tangent
	//   44 -> bitangent
	//
	// The last two entries are the extra tangent-space data needed by the shader
	// to turn a sampled normal-map vector into a world-space lighting normal.
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// A PSO in D3D12 is a large immutable bundle of GPU state.
	  // Instead of setting many tiny states dynamically like older APIs, D3D12
	  // expects most of the fixed-function configuration to be pre-baked here.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	// Main PSO: opaque + single-sided.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreatePointLightGizmoPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pointLightGizmoPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateShadowPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.DepthBias = ShadowRasterDepthBias;
	psoDesc.RasterizerState.SlopeScaledDepthBias = ShadowRasterSlopeScaledDepthBias;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	// This PSO has no pixel shader and no render targets because the shadow pass
	 // only writes depth into the shadow map.
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	// A small depth bias helps reduce "shadow acne" caused by limited depth precision.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedShadowPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	// Same idea as the main shadow PSO, but culling is disabled for double-sided meshes.
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.DepthBias = ShadowRasterDepthBias;
	psoDesc.RasterizerState.SlopeScaledDepthBias = ShadowRasterSlopeScaledDepthBias;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedShadowPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateTransparentPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	auto& transparentBlend = psoDesc.BlendState.RenderTarget[0];
	transparentBlend.BlendEnable = TRUE;
	transparentBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparentBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOp = D3D12_BLEND_OP_ADD;
	transparentBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparentBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparentBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Transparent PSO: transparent + single-sided.
	// Depth testing stays enabled, but depth writes are disabled so multiple
	// transparent layers can still contribute when drawn back-to-front.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_transparentPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	// Double-sided opaque PSO: same as the main opaque PSO except culling is off.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedTransparentPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	auto& transparentBlend = psoDesc.BlendState.RenderTarget[0];
	transparentBlend.BlendEnable = TRUE;
	transparentBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparentBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOp = D3D12_BLEND_OP_ADD;
	transparentBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparentBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparentBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Double-sided transparent PSO: transparent + no culling.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedTransparentPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateGridPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	// Load file shaders.hlsl (bạn cần tạo file này trong project)
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// The grid uses a separate PSO because it has different rendering needs:
	// line topology and depth writes disabled, while still depth-testing against
	// the rest of the scene.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gridPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// D3D12 requires CBVs to be 256-byte aligned.
	  // This buffer holds transforms that are updated every frame.
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_sceneDataConstantBuffer)));

	// UPLOAD heaps are CPU-visible, so we map once and keep the pointer alive.
	 // That makes per-frame updates very simple for learning purposes.
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_sceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pSceneDataCbvDataBegin)));
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
}

void ZenithRenderEngine::CreateShadowSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_shadowSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_shadowSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pShadowSceneDataCbvDataBegin)));
	memcpy(m_pShadowSceneDataCbvDataBegin, &m_shadowSceneDataCbData, sizeof(m_shadowSceneDataCbData));
}

void ZenithRenderEngine::CreatePointShadowSceneDataConstantBuffers()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pointShadowSceneDataConstantBuffers[faceIndex])));

		CD3DX12_RANGE readRange(0, 0);
		ThrowIfFailed(m_pointShadowSceneDataConstantBuffers[faceIndex]->Map(
			0,
			&readRange,
			reinterpret_cast<void**>(&m_pPointShadowSceneDataCbvDataBegin[faceIndex])));
		memcpy(
			m_pPointShadowSceneDataCbvDataBegin[faceIndex],
			&m_pointShadowSceneDataCbData[faceIndex],
			sizeof(m_pointShadowSceneDataCbData[faceIndex]));
	}
}

void ZenithRenderEngine::CreateLightingDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// Same idea as SceneData: small per-frame buffer, stored in an UPLOAD heap.
	UINT constantBufferSize = (sizeof(LightingDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_lightingDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_lightingDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pLightingDataCbvDataBegin)));
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::CreateGridVertexBuffer(float radius)
{
	// The grid is generated procedurally on the CPU instead of being loaded from
	 // a file. It is only a teaching / orientation aid, so an UPLOAD heap is good
	 // enough and keeps the code shorter than a full default-heap upload path.
	constexpr int halfLineCount = 10;
	const float spacing = (std::max)(1.0f, ceilf((std::max)(radius, 1.0f) / static_cast<float>(halfLineCount)));
	const float extent = static_cast<float>(halfLineCount) * spacing;
	constexpr float gridY = 0.0f;
	const XMFLOAT3 minorColor(0.25f, 0.25f, 0.25f);
	const XMFLOAT3 xAxisColor(0.55f, 0.20f, 0.20f);
	const XMFLOAT3 zAxisColor(0.20f, 0.35f, 0.60f);

	std::vector<GridVertex> vertices;
	vertices.reserve(static_cast<size_t>((halfLineCount * 2 + 1) * 4));

	for (int i = -halfLineCount; i <= halfLineCount; ++i)
	{
		const float offset = static_cast<float>(i) * spacing;
		const XMFLOAT3 lineColor = (i == 0) ? xAxisColor : minorColor;
		vertices.push_back({ XMFLOAT3(-extent, gridY, offset), lineColor });
		vertices.push_back({ XMFLOAT3(extent, gridY, offset), lineColor });
	}

	for (int i = -halfLineCount; i <= halfLineCount; ++i)
	{
		const float offset = static_cast<float>(i) * spacing;
		const XMFLOAT3 lineColor = (i == 0) ? zAxisColor : minorColor;
		vertices.push_back({ XMFLOAT3(offset, gridY, -extent), lineColor });
		vertices.push_back({ XMFLOAT3(offset, gridY, extent), lineColor });
	}

	m_gridVertexCount = static_cast<UINT>(vertices.size());
	const UINT bufferSize = static_cast<UINT>(vertices.size() * sizeof(GridVertex));
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_gridVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_gridVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices.data(), bufferSize);
	m_gridVertexBuffer->Unmap(0, nullptr);

	m_gridVertexBufferView.BufferLocation = m_gridVertexBuffer->GetGPUVirtualAddress();
	m_gridVertexBufferView.StrideInBytes = sizeof(GridVertex);
	m_gridVertexBufferView.SizeInBytes = bufferSize;
}

void ZenithRenderEngine::CreatePointLightGizmoVertexBuffer()
{
	const XMFLOAT3 color = m_isPointLightHovered ? MakePointLightHoverColor(m_pointLightColor) : m_pointLightColor;
	const GridVertex vertices[] = {
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color }
	};

	m_pointLightGizmoVertexCount = _countof(vertices);
	const UINT bufferSize = sizeof(vertices);
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_pointLightGizmoVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightGizmoVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, bufferSize);
	m_pointLightGizmoVertexBuffer->Unmap(0, nullptr);

	m_pointLightGizmoVertexBufferView.BufferLocation = m_pointLightGizmoVertexBuffer->GetGPUVirtualAddress();
	m_pointLightGizmoVertexBufferView.StrideInBytes = sizeof(GridVertex);
	m_pointLightGizmoVertexBufferView.SizeInBytes = bufferSize;
	UpdatePointLightGizmoVertexBuffer();
}

void ZenithRenderEngine::UpdatePointLightGizmoVertexBuffer()
{
	if (!m_pointLightGizmoVertexBuffer)
	{
		return;
	}

	const XMFLOAT3 color = m_isPointLightHovered ? MakePointLightHoverColor(m_pointLightColor) : m_pointLightColor;
	const GridVertex vertices[] = {
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f, -1.0f,  1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(1.0f,  1.0f,  1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f, -1.0f), color },
		{ XMFLOAT3(-1.0f,  1.0f,  1.0f), color }
	};

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightGizmoVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, sizeof(vertices));
	m_pointLightGizmoVertexBuffer->Unmap(0, nullptr);
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

	// Same idea as the directional-light sync helper: copy the renderer's current
	 // point-light state back into the dialog controls so sliders/text always match.
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
		// Layout strategy for the light dialogs:
		  //   label on the left
		  //   slider in the middle for fast changes
		  //   edit box on the right for exact values
		  // This keeps the UI simple while still teaching the difference between
		  // "interactive tweaking" and "precise numeric entry".
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

		// Trackbars send `WM_HSCROLL` continuously while the thumb moves, which is
		   // exactly what we want for live preview. Each branch updates one runtime
		   // parameter, syncs the matching edit box, and requests a redraw.
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

void ZenithRenderEngine::UpdatePointLightHoverState(int x, int y)
{
	if (!m_pointLightEnabled)
	{
		if (m_isPointLightHovered)
		{
			m_isPointLightHovered = false;
			UpdatePointLightGizmoVertexBuffer();
		}
		return;
	}

	RECT gizmoScreenBounds = {};
	bool isHovered = false;
	if (TryComputePointLightScreenBounds(
		m_pointLightPosition,
		m_pointLightGizmoScale,
		static_cast<float>(GetWidth()),
		static_cast<float>(GetHeight()),
		m_camera.GetViewMatrix(),
		m_camera.GetProjectionMatrix(),
		gizmoScreenBounds))
	{
		isHovered = IsScreenPointInsideBounds(static_cast<float>(x), static_cast<float>(y), gizmoScreenBounds, PointLightGizmoHitPaddingPixels);
	}

	if (isHovered != m_isPointLightHovered)
	{
		m_isPointLightHovered = isHovered;
		UpdatePointLightGizmoVertexBuffer();
	}
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
		// The point-light dialog uses the same label/slider/edit pattern so both
		  // light types feel consistent to someone exploring the renderer for the
		  // first time.
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

		// Live point-light preview: as the user drags a slider, the runtime value is
		 // updated immediately and the main viewport is asked to render another frame.
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

void ZenithRenderEngine::CreatePointLightSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_pointLightSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pPointLightSceneDataCbvDataBegin)));
	memcpy(m_pPointLightSceneDataCbvDataBegin, &m_pointLightSceneDataCbData, sizeof(m_pointLightSceneDataCbData));
}

void ZenithRenderEngine::CreateGroundPlaneVertexBuffer(float radius)
{
	// Build a simple two-triangle quad at y = 0 so shadows have a solid receiver.
	const float extent = (std::max)(10.0f, radius * 1.5f);
	const Vertex vertices[] = {
		{ XMFLOAT3(-extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) },
		{ XMFLOAT3(-extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) },
		{ XMFLOAT3(extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) },
		{ XMFLOAT3(-extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) },
		{ XMFLOAT3(extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) },
		{ XMFLOAT3(extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) },
	};

	m_groundPlaneVertexCount = _countof(vertices);
	const UINT bufferSize = sizeof(vertices);
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, bufferSize);
	m_groundPlaneVertexBuffer->Unmap(0, nullptr);

	m_groundPlaneVertexBufferView.BufferLocation = m_groundPlaneVertexBuffer->GetGPUVirtualAddress();
	m_groundPlaneVertexBufferView.StrideInBytes = sizeof(Vertex);
	m_groundPlaneVertexBufferView.SizeInBytes = bufferSize;
}

void ZenithRenderEngine::CreateGroundPlaneSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pGroundPlaneSceneDataCbvDataBegin)));
	memcpy(m_pGroundPlaneSceneDataCbvDataBegin, &m_groundPlaneSceneDataCbData, sizeof(m_groundPlaneSceneDataCbData));
}

void ZenithRenderEngine::CreateGroundPlaneMaterialConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(MaterialData) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneMaterialConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneMaterialConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pGroundPlaneMaterialCbvDataBegin)));

	// The plane is intentionally simple: no textures, just a constant gray tint.
	m_groundPlaneMaterialData = {};
	m_groundPlaneMaterialData.alphaMode = 0;
	m_groundPlaneMaterialData.baseColorFactor = XMFLOAT4(0.55f, 0.55f, 0.55f, 1.0f);
	memcpy(m_pGroundPlaneMaterialCbvDataBegin, &m_groundPlaneMaterialData, sizeof(m_groundPlaneMaterialData));
}

void ZenithRenderEngine::LoadModelFromPath(const std::wstring& path)
{
	if (path.empty())
	{
		return;
	}

	m_renderContext->WaitForGpu();
	// Destroy the old model only after the GPU is idle. This is conservative but
	// easy to reason about while learning resource lifetime in D3D12.
	m_model.reset();
	m_modelOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);

	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
	// Model construction records upload commands for vertex/index/texture data.
	   // We open the upload command list first, then let Model fill it.
	m_renderContext->BeginUpload();
	auto commandList = m_renderContext->GetCommandList();
	auto model = std::make_unique<Model>(cbvSrvUavAllocator, device, commandList, WideToUtf8(path));
	m_renderContext->EndUpload();

	if (!model->IsLoaded())
	{
		MessageBoxW(Win32Application::GetHwnd(), L"Failed to load the selected model.", GetTitle(), MB_OK | MB_ICONERROR);
		return;
	}

	const XMFLOAT3 boundsCenter = model->GetBoundsCenter();
	const XMFLOAT3 boundsMin = model->GetBoundsMin();
	const float boundsRadius = model->GetBoundsRadius();
	// The model is recentered so that:
	 //   - X/Z are centered around the origin
	 //   - the lowest Y point sits on the ground plane
	 // This makes orbiting and grid visualization feel much nicer.
	m_modelOffset = XMFLOAT3(-boundsCenter.x, -boundsMin.y, -boundsCenter.z);
	m_camera.FrameBoundingSphere(XMFLOAT3(0.0f, boundsCenter.y - boundsMin.y, 0.0f), boundsRadius);
	CreateGridVertexBuffer(boundsRadius);
	CreateGroundPlaneVertexBuffer(boundsRadius);
	model->ReleaseUploadBuffers();
	m_model = std::move(model);
	SetCustomWindowText(path.c_str());
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	// Update camera (smoothing / spring hook – currently instant)
	m_camera.Update();

	// Build the matrices that shaders need this frame.
		 //
		 // In this sample the world matrix is only a translation used to recenter the
		 // imported model, but this is exactly where rotation / scale / animation would
	   // be composed in a larger renderer.
		 //
		 // This function is also where all shadow-camera work is prepared:
		 //   - choose a light direction
		 //   - position a light camera
		 //   - fit an orthographic projection to the model bounds
		 //   - upload the final lightViewProjection matrix for the main shader
	const XMMATRIX translate = XMMatrixTranslation(
		m_modelOffset.x,
		m_modelOffset.y,
		m_modelOffset.z);
	XMMATRIX world = translate;

	const XMMATRIX view = m_camera.GetViewMatrix();
	const XMMATRIX proj = m_camera.GetProjectionMatrix();
	const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
	// Build a simple directional-light camera for shadow mapping.
	 //
	 // Think of this as placing a second camera in the world:
	 //   - the normal camera renders what the player sees
	 //   - the light camera renders what the light sees
   // The directional light direction now comes from the UI instead of being hard
	// coded. The stored value is treated as a raw editable vector in the -1..1
	// range, then normalized here before it is used as an actual light direction.
	XMFLOAT3 directionalLightDirection = m_directionalLightDirection;
	if (XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&directionalLightDirection))) < 1e-6f)
	{
		// If the UI ever produces an all-zero direction, fall back to the original
		  // tutorial light so the frame still renders with a sensible shadow setup.
		directionalLightDirection = XMFLOAT3(-0.2f, -1.0f, -0.3f);
	}

	XMFLOAT3 shadowTarget = XMFLOAT3(0.0f, 0.0f, 0.0f);
	float shadowBoundsRadius = 15.0f;
	XMFLOAT3 shadowBoundsMin = XMFLOAT3(-shadowBoundsRadius, 0.0f, -shadowBoundsRadius);
	XMFLOAT3 shadowBoundsMax = XMFLOAT3(shadowBoundsRadius, shadowBoundsRadius * 2.0f, shadowBoundsRadius);
	if (m_model)
	{
		const XMFLOAT3 boundsCenter = m_model->GetBoundsCenter();
		const XMFLOAT3 boundsMin = m_model->GetBoundsMin();
		const XMFLOAT3 boundsMax = m_model->GetBoundsMax();
		shadowTarget = XMFLOAT3(0.0f, boundsCenter.y - boundsMin.y, 0.0f);
		shadowBoundsRadius = (std::max)(m_model->GetBoundsRadius(), 1.0f);
		shadowBoundsMin = XMFLOAT3(
			boundsMin.x + m_modelOffset.x,
			boundsMin.y + m_modelOffset.y,
			boundsMin.z + m_modelOffset.z);
		shadowBoundsMax = XMFLOAT3(
			boundsMax.x + m_modelOffset.x,
			boundsMax.y + m_modelOffset.y,
			boundsMax.z + m_modelOffset.z);
	}

	const XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&directionalLightDirection));
	const XMVECTOR shadowTargetVector = XMLoadFloat3(&shadowTarget);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetX(XMVector3Dot(lightDirection, lightUp))) > 0.99f)
	{
		lightUp = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	}

	const XMVECTOR lightPosition = shadowTargetVector - lightDirection * (shadowBoundsRadius * 2.5f);
	const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, shadowTargetVector, lightUp);

	// Fit the orthographic shadow camera to the model's transformed bounds in light space.
	   // This uses the shadow-map texels more efficiently than a loose radius-based box.
	   //
	   // Why do this?
	   //   - a loose box wastes shadow resolution on empty space
	   //   - a tighter box gives sharper shadows from the same texture size
	   //
	   // The idea is simple:
	   //   1. build the 8 corners of the model's world-space bounding box
	   //   2. transform those corners into light space
	   //   3. compute min/max extents there
	   //   4. build an off-center orthographic projection that tightly encloses them
	const XMFLOAT3 shadowCorners[] = {
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMin.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMin.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMax.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMax.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMin.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMin.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMax.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMax.y, shadowBoundsMax.z)
	};

	float minLightX = FLT_MAX;
	float minLightY = FLT_MAX;
	float minLightZ = FLT_MAX;
	float maxLightX = -FLT_MAX;
	float maxLightY = -FLT_MAX;
	float maxLightZ = -FLT_MAX;
	for (const XMFLOAT3& corner : shadowCorners)
	{
		const XMVECTOR lightSpaceCorner = XMVector3TransformCoord(XMLoadFloat3(&corner), lightView);
		const float lightX = XMVectorGetX(lightSpaceCorner);
		const float lightY = XMVectorGetY(lightSpaceCorner);
		const float lightZ = XMVectorGetZ(lightSpaceCorner);
		minLightX = (std::min)(minLightX, lightX);
		minLightY = (std::min)(minLightY, lightY);
		minLightZ = (std::min)(minLightZ, lightZ);
		maxLightX = (std::max)(maxLightX, lightX);
		maxLightY = (std::max)(maxLightY, lightY);
		maxLightZ = (std::max)(maxLightZ, lightZ);
	}

	const float xyPadding = (std::max)(ShadowMinFrustumPadding, shadowBoundsRadius * 0.15f);
	const float zPadding = (std::max)(ShadowMinFrustumPadding * 4.0f, shadowBoundsRadius * ShadowDepthRangePaddingScale);
	const float nearPlane = (std::max)(0.1f, minLightZ - zPadding);
	const float farPlane = (std::max)(nearPlane + 1.0f, maxLightZ + zPadding);
	const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
		minLightX - xyPadding,
		maxLightX + xyPadding,
		minLightY - xyPadding,
		maxLightY + xyPadding,
		nearPlane,
		farPlane);
	const XMMATRIX lightViewProjection = lightView * lightProjection;

	// HLSL constant buffers are treated as column-major here, so the matrices are
	  // transposed before upload to match shader-side multiplication.
	XMStoreFloat4x4(&m_sceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_sceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_sceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_sceneDataCbData.normalMatrix, normalMat);
	const float pointLightSceneScale = (std::max)(1.0f, shadowBoundsRadius);
	m_pointLightGizmoScale = (std::min)(
		PointLightGizmoScaleMax,
		(std::max)(PointLightGizmoScaleMin, pointLightSceneScale * PointLightGizmoSceneScaleFactor));
	const XMMATRIX pointLightWorld =
		XMMatrixScaling(m_pointLightGizmoScale, m_pointLightGizmoScale, m_pointLightGizmoScale) *
		XMMatrixTranslation(m_pointLightPosition.x, m_pointLightPosition.y, m_pointLightPosition.z);
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.model, XMMatrixTranspose(pointLightWorld));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.normalMatrix, XMMatrixTranspose(XMMatrixIdentity()));
	// The ground plane stays at the origin instead of using the model recenter offset.
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.model, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.normalMatrix, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.view, XMMatrixTranspose(lightView));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.projection, XMMatrixTranspose(lightProjection));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.normalMatrix, normalMat);

	const XMVECTOR pointLightPositionVector = XMLoadFloat3(&m_pointLightPosition);
	const float pointShadowFarPlane = (std::max)(m_pointLightRange, PointShadowNearPlane + 0.1f);
	const XMMATRIX pointShadowProjection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, PointShadowNearPlane, pointShadowFarPlane);
	const XMVECTOR pointShadowDirections[PointShadowFaceCount] = {
		XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet(-1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f)
	};
	const XMVECTOR pointShadowUps[PointShadowFaceCount] = {
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
	};

	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		const XMMATRIX pointShadowView = XMMatrixLookAtLH(
			pointLightPositionVector,
			pointLightPositionVector + pointShadowDirections[faceIndex],
			pointShadowUps[faceIndex]);
		const XMMATRIX pointShadowViewProjection = pointShadowView * pointShadowProjection;

		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].model, XMMatrixTranspose(world));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].view, XMMatrixTranspose(pointShadowView));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].projection, XMMatrixTranspose(pointShadowProjection));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].normalMatrix, normalMat);
		XMStoreFloat4x4(&m_lightingDataCbData.pointLightShadowViewProjection[faceIndex], XMMatrixTranspose(pointShadowViewProjection));
	}

	XMStoreFloat4(&m_lightingDataCbData.viewPosition, m_camera.GetPosition());

	// Strength and exposure are split so beginners can experiment with a familiar
	// "base power + exposure stop" workflow:
	//   finalIntensity = strength * 2^exposure
	// This mirrors how the point light is controlled and keeps both lights
	// conceptually consistent.
	const float directionalLightIntensity = m_directionalLightStrength * powf(2.0f, m_directionalLightExposure);
	m_lightingDataCbData.directionalLight.direction = XMFLOAT4(directionalLightDirection.x, directionalLightDirection.y, directionalLightDirection.z, 1.0f);
	if (m_directionalLightEnabled)
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.2f * directionalLightIntensity, 0.2f * directionalLightIntensity, 0.2f * directionalLightIntensity, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.5f * directionalLightIntensity, 0.5f * directionalLightIntensity, 0.5f * directionalLightIntensity, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(1.0f * directionalLightIntensity, 1.0f * directionalLightIntensity, 1.0f * directionalLightIntensity, 1.0f);
	}
	else
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	// This matrix is the bridge between the main pass and the shadow map.
	   // Each main-pass pixel transforms itself into this light space to test shadowing.
	XMStoreFloat4x4(&m_lightingDataCbData.lightViewProjection, XMMatrixTranspose(lightViewProjection));
	m_lightingDataCbData.shadowParams = XMFLOAT4(
		static_cast<float>(m_renderContext->GetShadowMapSrvIndex()),
		ShadowComparisonBias,
		FlipNormalMapGreenChannel ? -1.0f : 1.0f,
		m_directionalLightEnabled ? 1.0f : 0.0f);
	m_lightingDataCbData.pointLightPosition = XMFLOAT4(
		m_pointLightPosition.x,
		m_pointLightPosition.y,
		m_pointLightPosition.z,
		m_pointLightRange);
	m_lightingDataCbData.pointLightColor = XMFLOAT4(
		m_pointLightColor.x,
		m_pointLightColor.y,
		m_pointLightColor.z,
		m_pointLightEnabled ? (m_pointLightStrength * powf(2.0f, m_pointLightExposure)) : 0.0f);
	m_lightingDataCbData.pointShadowParams = XMFLOAT4(
		static_cast<float>(m_renderContext->GetPointShadowMapSrvIndex(0)),
		PointShadowComparisonBias,
		pointShadowFarPlane,
		m_pointLightEnabled ? 1.0f : 0.0f);

	// Because both constant buffers live in persistently mapped UPLOAD heaps,
	// updating them is just a memcpy into the mapped CPU pointer.
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
	memcpy(m_pPointLightSceneDataCbvDataBegin, &m_pointLightSceneDataCbData, sizeof(m_pointLightSceneDataCbData));
	memcpy(m_pGroundPlaneSceneDataCbvDataBegin, &m_groundPlaneSceneDataCbData, sizeof(m_groundPlaneSceneDataCbData));
	memcpy(m_pShadowSceneDataCbvDataBegin, &m_shadowSceneDataCbData, sizeof(m_shadowSceneDataCbData));
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		memcpy(
			m_pPointShadowSceneDataCbvDataBegin[faceIndex],
			&m_pointShadowSceneDataCbData[faceIndex],
			sizeof(m_pointShadowSceneDataCbData[faceIndex]));
	}
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	std::wstring capturePath = std::move(m_pendingRenderImagePath);
	m_pendingRenderImagePath.clear();

	// 1. Prepare the command list for rendering (Reset command allocator, command list, set render target, etc.)
	 //
	 // Important rendering order for the whole shadow system:
	 //   1. prepare command list and per-frame descriptors
	 //   2. render shadow map from the light's point of view
	 //   3. render the main scene from the camera's point of view
	 //   4. present the back buffer
	m_renderContext->Prepare();

	auto commandList = m_renderContext->GetCommandList();

	// 2. Bind the root signature and create this frame's temporary CBV descriptors.
	  //
	  // The descriptor allocator already copied all static descriptors (mainly model
	  // textures) into the current shader-visible heap during Prepare().
	  // Now we append temporary per-frame descriptors after that static range.
	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
	const UINT shadowSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	UINT pointShadowSceneDescriptorIndices[PointShadowFaceCount] = {};
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		pointShadowSceneDescriptorIndices[faceIndex] = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	}
	const UINT pointLightSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT groundPlaneSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT sceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT lightingDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	// The shadow pass uses its own SceneData descriptor because it renders from the light.
	D3D12_CONSTANT_BUFFER_VIEW_DESC shadowSceneCbvDesc = {};
	shadowSceneCbvDesc.BufferLocation = m_shadowSceneDataConstantBuffer->GetGPUVirtualAddress();
	shadowSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&shadowSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(shadowSceneDescriptorIndex));
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC pointShadowSceneCbvDesc = {};
		pointShadowSceneCbvDesc.BufferLocation = m_pointShadowSceneDataConstantBuffers[faceIndex]->GetGPUVirtualAddress();
		pointShadowSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
		device->CreateConstantBufferView(&pointShadowSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(pointShadowSceneDescriptorIndices[faceIndex]));
	}
	D3D12_CONSTANT_BUFFER_VIEW_DESC pointLightSceneCbvDesc = {};
	pointLightSceneCbvDesc.BufferLocation = m_pointLightSceneDataConstantBuffer->GetGPUVirtualAddress();
	pointLightSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&pointLightSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(pointLightSceneDescriptorIndex));
	// The ground plane also has separate SceneData because its world transform differs
	 // from the model's transform.
	D3D12_CONSTANT_BUFFER_VIEW_DESC groundPlaneSceneCbvDesc = {};
	groundPlaneSceneCbvDesc.BufferLocation = m_groundPlaneSceneDataConstantBuffer->GetGPUVirtualAddress();
	groundPlaneSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&groundPlaneSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(groundPlaneSceneDescriptorIndex));
	D3D12_CONSTANT_BUFFER_VIEW_DESC sceneCbvDesc = {};
	sceneCbvDesc.BufferLocation = m_sceneDataConstantBuffer->GetGPUVirtualAddress();
	sceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&sceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(sceneDescriptorIndex));
	D3D12_CONSTANT_BUFFER_VIEW_DESC lightingCbvDesc = {};
	lightingCbvDesc.BufferLocation = m_lightingDataConstantBuffer->GetGPUVirtualAddress();
	lightingCbvDesc.SizeInBytes = (sizeof(LightingDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&lightingCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(lightingDescriptorIndex));

	// Bind the single shader-visible CBV/SRV/UAV heap for this frame, then bind
	 // root tables by index into that heap.
	ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavAllocator->GetShaderVisibleHeap() };
	commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	commandList->SetGraphicsRootDescriptorTable(0, cbvSrvUavAllocator->GetDynamicGpuHandle(0));

	// 3. At this point all root bindings needed by the shaders are in place:
	 //   slot 0 -> texture SRV table
	 //   slot 1 -> scene CBV table
	 //   slot 2 -> material root CBV (set later per mesh)
	 //   slot 3 -> lighting CBV table
	 //
	 // The shadow pass reuses the same root signature layout. The only difference is
	 // that slot 1 points at the light-camera SceneData instead of the normal camera.
	if (m_model)
	{
		// --- Shadow pass ---
		   // 1. Turn the shadow map from a sampled texture into a writable depth target.
		auto shadowToDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		commandList->ResourceBarrier(1, &shadowToDepthWrite);

		const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle = m_renderContext->GetShadowMapDsv();
		// 2. Bind the light's camera matrices, shadow viewport, and shadow DSV.
		commandList->SetGraphicsRootSignature(m_rootSignature.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(shadowSceneDescriptorIndex));
		commandList->RSSetViewports(1, &m_renderContext->GetShadowMapViewport());
		commandList->RSSetScissorRects(1, &m_renderContext->GetShadowMapScissorRect());
		commandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);

		// 3. Render geometry from the light's point of view.
		//
		// The shadow PSO is depth-focused, so no color is written here.
		// Instead, the GPU stores the nearest surface depth into the shadow map.
		// Later, the main shader compares its current pixel depth against that value.
		commandList->SetPipelineState(m_shadowPipelineState.Get());
		m_model->DrawOpaque(commandList, false);
		commandList->SetPipelineState(m_doubleSidedShadowPipelineState.Get());
		m_model->DrawOpaque(commandList, true);

		// 4. Turn the shadow map back into a shader-readable texture for the main pass.
		auto shadowToPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &shadowToPixelShaderResource);

		if (m_pointLightEnabled)
		{
			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				auto pointShadowToDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
					m_renderContext->GetPointShadowMap(faceIndex),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_DEPTH_WRITE);
				commandList->ResourceBarrier(1, &pointShadowToDepthWrite);
			}

			commandList->SetGraphicsRootSignature(m_rootSignature.Get());
			commandList->RSSetViewports(1, &m_renderContext->GetPointShadowMapViewport());
			commandList->RSSetScissorRects(1, &m_renderContext->GetPointShadowMapScissorRect());

			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				const D3D12_CPU_DESCRIPTOR_HANDLE pointShadowDsvHandle = m_renderContext->GetPointShadowMapDsv(faceIndex);
				commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(pointShadowSceneDescriptorIndices[faceIndex]));
				commandList->ClearDepthStencilView(pointShadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				commandList->OMSetRenderTargets(0, nullptr, FALSE, &pointShadowDsvHandle);

				commandList->SetPipelineState(m_shadowPipelineState.Get());
				m_model->DrawOpaque(commandList, false);
				commandList->SetPipelineState(m_doubleSidedShadowPipelineState.Get());
				m_model->DrawOpaque(commandList, true);
			}

			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				auto pointShadowToPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
					m_renderContext->GetPointShadowMap(faceIndex),
					D3D12_RESOURCE_STATE_DEPTH_WRITE,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				commandList->ResourceBarrier(1, &pointShadowToPixelShaderResource);
			}
		}
	}

	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	commandList->SetGraphicsRootDescriptorTable(3, cbvSrvUavAllocator->GetDynamicGpuHandle(lightingDescriptorIndex));

	// 4. Set up scissor rect and viewport
	commandList->RSSetViewports(1, &m_viewport);
	commandList->RSSetScissorRects(1, &m_scissorRect);

	// 5. Select the current back buffer and depth buffer, then clear them.
	   // The RTV handle is offset by the current swap-chain frame index because each
	   // back buffer has its own render-target descriptor.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		m_renderContext->GetRtvHeapStart(), // Bạn cần thêm Getter này trong RenderContext
		m_renderContext->GetCurrentFrameIndex(),
		m_renderContext->GetRtvDescriptorSize()
	);
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_renderContext->GetDsvHeapStart());

	const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	if (m_useSolidGroundPlane)
	{
		// Draw the debugging ground plane with the normal lit shader so it receives shadows.
		commandList->SetPipelineState(m_doubleSidedPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(groundPlaneSceneDescriptorIndex));
		commandList->SetGraphicsRootConstantBufferView(2, m_groundPlaneMaterialConstantBuffer->GetGPUVirtualAddress());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->IASetVertexBuffers(0, 1, &m_groundPlaneVertexBufferView);
		commandList->DrawInstanced(m_groundPlaneVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}
	else
	{
		// Draw grid first so the viewer has visual orientation even before a model is loaded.
		commandList->SetPipelineState(m_gridPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(groundPlaneSceneDescriptorIndex));
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		commandList->IASetVertexBuffers(0, 1, &m_gridVertexBufferView);
		commandList->DrawInstanced(m_gridVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}

	// Opaque meshes render first so they fill depth normally.
	  // At this point the shadow map is already ready for sampling in the pixel shader.
	commandList->SetPipelineState(m_pipelineState.Get());

	// 6. Draw the model
	if (m_model) {
		// Draw order matters:
		//   1. opaque single-sided
		//   2. opaque double-sided
		//   3. transparent single-sided
		//   4. transparent double-sided
		//
		// This keeps the common opaque path fast and predictable while still
		// honoring glTF doubleSided on the meshes that actually request it.
		m_model->DrawOpaque(commandList, false);
		commandList->SetPipelineState(m_doubleSidedPipelineState.Get());
		m_model->DrawOpaque(commandList, true);

		XMFLOAT3 cameraPosition;
		XMStoreFloat3(&cameraPosition, m_camera.GetPosition());
		commandList->SetPipelineState(m_transparentPipelineState.Get());
		m_model->DrawTransparent(commandList, cameraPosition, m_modelOffset, false);
		commandList->SetPipelineState(m_doubleSidedTransparentPipelineState.Get());
		m_model->DrawTransparent(commandList, cameraPosition, m_modelOffset, true);
	}

    // The point-light cube is an editor gizmo, not part of the final beauty render.
	// Keep it visible in the interactive viewport, but skip it when the user is
	// exporting an image so the saved result contains only scene lighting.
	if (m_pointLightEnabled && capturePath.empty())
	{
		commandList->SetPipelineState(m_pointLightGizmoPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(pointLightSceneDescriptorIndex));
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		commandList->IASetVertexBuffers(0, 1, &m_pointLightGizmoVertexBufferView);
		commandList->DrawInstanced(m_pointLightGizmoVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}

	// 7. Present() closes and executes the command list, then presents the swap chain.
	const bool captureSucceeded = m_renderContext->Present(capturePath);
	if (!capturePath.empty() && !captureSucceeded)
	{
		MessageBoxW(
			Win32Application::GetHwnd(),
			L"Failed to save the rendered image.",
			GetTitle(),
			MB_OK | MB_ICONERROR);
	}
}

void ZenithRenderEngine::OnDestroy()
{
	// Make sure to wait for the GPU to finish all pending work before exiting to avoid access violations and ensure proper cleanup of resources.
	m_renderContext->WaitForGpu();
	DestroyDirectionalLightConfigWindow();
	DestroyPointLightConfigWindow();
}

// ---------------------------------------------------------------------------
// Mouse handlers – delegate straight to the Camera
// ---------------------------------------------------------------------------

void ZenithRenderEngine::OnLeftButtonDown(int x, int y)
{
	m_lastLeftMousePosition.x = x;
	m_lastLeftMousePosition.y = y;
	if (!m_pointLightEnabled)
	{
		m_isPointLightDragging = false;
		return;
	}

	const float viewportWidth = static_cast<float>(GetWidth());
	const float viewportHeight = static_cast<float>(GetHeight());
	const XMMATRIX view = m_camera.GetViewMatrix();
	const XMMATRIX projection = m_camera.GetProjectionMatrix();

	RECT gizmoScreenBounds = {};
	if (!TryComputePointLightScreenBounds(m_pointLightPosition, m_pointLightGizmoScale, viewportWidth, viewportHeight, view, projection, gizmoScreenBounds))
	{
		if (m_isPointLightHovered)
		{
			m_isPointLightHovered = false;
			UpdatePointLightGizmoVertexBuffer();
		}
		m_isPointLightDragging = false;
		return;
	}

	if (!IsScreenPointInsideBounds(static_cast<float>(x), static_cast<float>(y), gizmoScreenBounds, PointLightGizmoHitPaddingPixels))
	{
		if (m_isPointLightHovered)
		{
			m_isPointLightHovered = false;
			UpdatePointLightGizmoVertexBuffer();
		}
		m_isPointLightDragging = false;
		return;
	}

	if (!m_isPointLightHovered)
	{
		m_isPointLightHovered = true;
		UpdatePointLightGizmoVertexBuffer();
	}

	MouseRay ray;
	if (!TryBuildMouseRay(x, y, viewportWidth, viewportHeight, view, projection, ray))
	{
		m_isPointLightDragging = false;
		return;
	}

	const XMVECTOR inverseView = XMMatrixInverse(nullptr, view).r[0];
	UNREFERENCED_PARAMETER(inverseView);
	const XMMATRIX viewInverse = XMMatrixInverse(nullptr, view);
	const XMVECTOR dragPlaneNormal = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), viewInverse));
	const XMVECTOR dragPlanePoint = XMLoadFloat3(&m_pointLightPosition);

	XMVECTOR hitPoint;
	if (!TryIntersectPlane(ray.origin, ray.direction, dragPlanePoint, dragPlaneNormal, hitPoint))
	{
		m_isPointLightDragging = false;
		return;
	}

	XMStoreFloat3(&m_pointLightDragPlanePoint, dragPlanePoint);
	XMStoreFloat3(&m_pointLightDragPlaneNormal, dragPlaneNormal);
	XMStoreFloat3(&m_pointLightDragOffset, dragPlanePoint - hitPoint);
	m_isPointLightDragging = true;
}

void ZenithRenderEngine::OnLeftButtonUp(int x, int y)
{
	m_isPointLightDragging = false;
	m_lastLeftMousePosition.x = x;
	m_lastLeftMousePosition.y = y;
}

void ZenithRenderEngine::OnMiddleButtonDown(int x, int y)
{
	m_camera.OnMiddleButtonDown(x, y);
}

void ZenithRenderEngine::OnMiddleButtonUp(int x, int y)
{
	m_camera.OnMiddleButtonUp();
}

void ZenithRenderEngine::OnMouseMove(int x, int y, WPARAM btnState)
{
	if (m_isPointLightDragging)
	{
		const int deltaX = x - m_lastLeftMousePosition.x;
		const int deltaY = y - m_lastLeftMousePosition.y;
		m_lastLeftMousePosition.x = x;
		m_lastLeftMousePosition.y = y;

		if ((btnState & MK_LBUTTON) == 0)
		{
			m_isPointLightDragging = false;
			return;
		}

		const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		if (shiftDown)
		{
			UNREFERENCED_PARAMETER(deltaX);
			const float cameraDistanceToLight = XMVectorGetX(XMVector3Length(m_camera.GetPosition() - XMLoadFloat3(&m_pointLightPosition)));
			const float worldUnitsPerPixel = (std::max)(0.01f, cameraDistanceToLight * PointLightVerticalDragSensitivity);
			m_pointLightPosition.y -= static_cast<float>(deltaY) * worldUnitsPerPixel;
			return;
		}

		MouseRay ray;
		if (TryBuildMouseRay(x, y, static_cast<float>(GetWidth()), static_cast<float>(GetHeight()), m_camera.GetViewMatrix(), m_camera.GetProjectionMatrix(), ray))
		{
			const XMVECTOR dragPlanePoint = XMLoadFloat3(&m_pointLightDragPlanePoint);
			const XMVECTOR dragPlaneNormal = XMLoadFloat3(&m_pointLightDragPlaneNormal);
			const XMVECTOR dragOffset = XMLoadFloat3(&m_pointLightDragOffset);
			XMVECTOR hitPoint;
			if (TryIntersectPlane(ray.origin, ray.direction, dragPlanePoint, dragPlaneNormal, hitPoint))
			{
				XMStoreFloat3(&m_pointLightPosition, hitPoint + dragOffset);
			}
		}

		return;
	}

	UpdatePointLightHoverState(x, y);

	// Shift held = pan, otherwise orbit
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	m_camera.OnMouseMove(x, y, shiftDown);
}

void ZenithRenderEngine::OnMouseWheel(float wheelDelta)
{
	m_camera.OnMouseWheel(wheelDelta);
}