#pragma once

namespace RenderEngineDetail
{
  // Shared constants/helpers for the renderer live in this header so multiple
	// source files can use the same UI IDs and math utilities without duplication.
	constexpr wchar_t DirectionalLightConfigWindowClassName[] = L"ZenithDirectionalLightConfigWindow";
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

	inline std::string WideToUtf8(const std::wstring& value)
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

	inline bool TryBuildMouseRay(int x, int y, float viewportWidth, float viewportHeight, FXMMATRIX view, CXMMATRIX projection, MouseRay& ray)
	{
        // Unprojecting one point at z=0 and one at z=1 converts a 2D mouse position
		// into a 3D world-space picking ray.
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

	inline bool TryIntersectPlane(FXMVECTOR rayOrigin, FXMVECTOR rayDirection, FXMVECTOR planePoint, FXMVECTOR planeNormal, XMVECTOR& hitPoint)
	{
        // Plane intersection is enough for the point-light drag tool because the gizmo
		// movement is constrained to a chosen edit plane rather than full 3D free-move.
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

	inline bool TryProjectWorldToScreen(FXMVECTOR worldPosition, float viewportWidth, float viewportHeight, FXMMATRIX view, CXMMATRIX projection, XMFLOAT2& screenPosition)
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

	inline bool TryComputePointLightScreenBounds(
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

	inline bool IsScreenPointInsideBounds(float x, float y, const RECT& bounds, float paddingPixels)
	{
		return x >= static_cast<float>(bounds.left) - paddingPixels &&
			x <= static_cast<float>(bounds.right) + paddingPixels &&
			y >= static_cast<float>(bounds.top) - paddingPixels &&
			y <= static_cast<float>(bounds.bottom) + paddingPixels;
	}

	inline std::wstring FormatFloatText(float value)
	{
		wchar_t buffer[64] = {};
		swprintf_s(buffer, L"%.2f", value);
		return std::wstring(buffer);
	}

	inline bool TryParseFloatFromWindow(HWND window, float& value)
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

	inline XMFLOAT3 MakePointLightHoverColor(const XMFLOAT3& baseColor)
	{
		return XMFLOAT3(
			(std::min)(1.0f, baseColor.x * 0.65f + 0.35f),
			(std::min)(1.0f, baseColor.y * 0.65f + 0.35f),
			(std::min)(1.0f, baseColor.z * 0.65f + 0.35f));
	}

	inline COLORREF FloatColorToColorRef(const XMFLOAT3& color)
	{
		const BYTE red = static_cast<BYTE>((std::clamp)(color.x, 0.0f, 1.0f) * 255.0f);
		const BYTE green = static_cast<BYTE>((std::clamp)(color.y, 0.0f, 1.0f) * 255.0f);
		const BYTE blue = static_cast<BYTE>((std::clamp)(color.z, 0.0f, 1.0f) * 255.0f);
		return RGB(red, green, blue);
	}

	inline XMFLOAT3 ColorRefToFloatColor(COLORREF color)
	{
		return XMFLOAT3(
			static_cast<float>(GetRValue(color)) / 255.0f,
			static_cast<float>(GetGValue(color)) / 255.0f,
			static_cast<float>(GetBValue(color)) / 255.0f);
	}

	inline int FloatToSliderPosition(float value, int sliderMin, int sliderMax)
	{
		const int scaledValue = static_cast<int>(lroundf(value * static_cast<float>(LightSliderScale)));
		return (std::clamp)(scaledValue, sliderMin, sliderMax);
	}

	inline float SliderPositionToFloat(int sliderPosition)
	{
		return static_cast<float>(sliderPosition) / static_cast<float>(LightSliderScale);
	}
}
