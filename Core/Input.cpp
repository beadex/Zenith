#include "pch.h"
#include "RenderEngine.h"
#include "Shared.h"

using namespace RenderEngineDetail;

void ZenithRenderEngine::UpdatePointLightHoverState(int x, int y)
{
   // The gizmo is picked in screen space instead of with a full triangle-level
	// ray test. For this simple cube handle, a projected screen rectangle is enough.
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
    // The drag plane is aligned to the camera so horizontal mouse motion feels like
	// moving the light across the screen rather than through depth.
	const XMVECTOR dragPlaneNormal = XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), viewInverse));
	const XMVECTOR dragPlanePoint = XMLoadFloat3(&m_pointLightPosition);

	XMVECTOR hitPoint;
	if (!TryIntersectPlane(ray.origin, ray.direction, dragPlanePoint, dragPlaneNormal, hitPoint))
	{
		m_isPointLightDragging = false;
		return;
	}

	XMStoreFloat3(&m_pointLightDragPlanePoint, dragPlanePoint);
   // Storing the initial hit offset prevents the gizmo from snapping its center
	// directly under the cursor when dragging begins.
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
            // Holding Shift switches to a simpler vertical-only edit mode. The amount
			// moved per pixel scales with distance so dragging feels similar when zoomed in or out.
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

   // If the point light is not being dragged, mouse motion falls back to camera control.
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	m_camera.OnMouseMove(x, y, shiftDown);
}

void ZenithRenderEngine::OnMouseWheel(float wheelDelta)
{
	m_camera.OnMouseWheel(wheelDelta);
}
