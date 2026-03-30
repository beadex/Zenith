#include "pch.h"
#include "Camera.h"
#include <algorithm>

using std::max;

Camera::Camera()
    : m_theta(0.0f)               // facing front (along -Z toward origin)
    , m_phi(XM_PI * 0.25f)        // slightly above horizon
    , m_radius(8.0f)
    , m_focusRadius(0.0f)
    , m_target(0.0f, 0.0f, 0.0f)
    , m_fovY(XM_PIDIV4)
    , m_aspect(16.0f / 9.0f)
    , m_nearZ(0.1f)
    , m_farZ(500.0f)
    , m_mmbDown(false)
    , m_lastMouseX(0)
    , m_lastMouseY(0)
{
}

void Camera::SetLens(float fovY, float aspectRatio, float nearZ, float farZ)
{
    m_fovY = fovY;
    m_aspect = aspectRatio;
    m_nearZ = nearZ;
    m_farZ = farZ;

    UpdateClipPlanes();
}

void Camera::FrameBoundingSphere(const XMFLOAT3& center, float radius)
{
    m_target = center;
    m_focusRadius = max(radius, 0.1f);

    const float halfFovY = m_fovY * 0.5f;
    const float halfFovX = atanf(tanf(halfFovY) * m_aspect);
    const float limitingHalfFov = (std::min)(halfFovX, halfFovY);

    m_radius = (m_focusRadius / tanf(limitingHalfFov)) * 1.2f;
    UpdateClipPlanes();
}

void Camera::OnMiddleButtonDown(int x, int y)
{
    m_mmbDown = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
}

void Camera::OnMiddleButtonUp()
{
    m_mmbDown = false;
}

void Camera::OnMouseMove(int x, int y, bool shiftDown)
{
    if (!m_mmbDown)
        return;

    const float dx = static_cast<float>(x - m_lastMouseX);
    const float dy = static_cast<float>(y - m_lastMouseY);
    m_lastMouseX = x;
    m_lastMouseY = y;

    if (shiftDown)
    {
        const float cosP = cosf(m_phi);
        const float sinP = sinf(m_phi);
        const float cosT = cosf(m_theta);
        const float sinT = sinf(m_theta);

        const XMVECTOR eyeOffset = XMVectorSet(
            m_radius * cosP * sinT,
            m_radius * sinP,
            m_radius * cosP * cosT,
            0.0f);
        const XMVECTOR target = XMLoadFloat3(&m_target);
        const XMVECTOR eye = XMVectorAdd(target, eyeOffset);
        const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(target, eye));
        const XMVECTOR up = XMVectorSet(0.0f, cosP >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

        const float panScale = kPanSpeed * max(m_radius, 1.0f);
        const XMVECTOR panOffset =
            (-dx * panScale) * right +
            (dy * panScale) * up;

        XMStoreFloat3(&m_target, XMVectorAdd(target, panOffset));
        return;
    }

    // Horizontal: drag right → camera sweeps right around model → theta decreases
    m_theta += dx * kOrbitSpeed;

    // Vertical: drag up (dy < 0 in screen space) → camera goes up → phi increases
    // No clamping – full 360° allowed in both directions
    m_phi -= dy * kOrbitSpeed;
}

void Camera::OnMouseWheel(float wheelDelta)
{
    const float notches = wheelDelta / static_cast<float>(WHEEL_DELTA);
    m_radius *= (1.0f - notches * kZoomSpeed);
    m_radius = max(0.1f, m_radius);
    UpdateClipPlanes();
}

void Camera::Update()
{
    UpdateClipPlanes();
}

void Camera::UpdateClipPlanes()
{
    if (m_focusRadius <= 0.0f)
        return;

    const float padding = m_focusRadius * 0.5f;
    m_nearZ = max(0.01f, m_radius - m_focusRadius - padding);
    m_farZ = (std::max)(m_nearZ + 1.0f, m_radius + m_focusRadius + padding);
}

XMVECTOR Camera::GetPosition() const
{
    // Standard spherical → Cartesian, phi measured from XZ plane (elevation)
    // phi=0  → on XZ plane
    // phi=PI/2  → straight up (+Y)
    // phi=PI    → back on XZ plane (but upside-down relative to start)
    const float cosP = cosf(m_phi);
    const float sinP = sinf(m_phi);
    const float cosT = cosf(m_theta);
    const float sinT = sinf(m_theta);

    return XMVectorAdd(
        XMLoadFloat3(&m_target),
        XMVectorSet(
            m_radius * cosP * sinT,
            m_radius * sinP,
            m_radius * cosP * cosT,
            0.0f));
}

XMMATRIX Camera::GetViewMatrix() const
{
    const XMVECTOR eye = GetPosition();
    const XMVECTOR target = XMLoadFloat3(&m_target);

    // When camera is in the upper hemisphere (cosP >= 0), world +Y is up.
    // When in the lower hemisphere (cosP < 0, gone over the top), flip to -Y
    // so the image doesn't suddenly invert – exactly what Blender does.
    const float cosP = cosf(m_phi);
    const XMVECTOR up = XMVectorSet(0.0f, cosP >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);

    return XMMatrixLookAtLH(eye, target, up);
}

XMMATRIX Camera::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
}