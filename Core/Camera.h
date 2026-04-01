#pragma once

#include <DirectXMath.h>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Camera – Blender-style turntable orbit around a target point.
//
//  Orbit  : Hold MMB + drag
//  Pan    : Hold Shift + MMB + drag  (coming soon)
//  Zoom   : Scroll wheel
//
// Internally uses plain spherical coordinates (theta, phi, radius).
// Simple, stable, no quaternion drift, no gimbal surprises.
// ---------------------------------------------------------------------------
class Camera
{
public:
    static constexpr float kOrbitSpeed = 0.008f;   // radians per pixel
    static constexpr float kPanSpeed = 0.003f;   // world-units per pixel (scaled by radius)
    static constexpr float kZoomSpeed = 0.12f;    // fraction of radius per scroll notch

    Camera();

    void SetLens(float fovY, float aspectRatio, float nearZ, float farZ);
    void FrameBoundingSphere(const XMFLOAT3& center, float radius);

    // Input
    void OnMiddleButtonDown(int x, int y);
    void OnMiddleButtonUp();
    void OnMouseMove(int x, int y, bool shiftDown);
    void OnMouseWheel(float wheelDelta);

    void Update();

    // Matrices
    XMMATRIX GetViewMatrix()       const;
    XMMATRIX GetProjectionMatrix() const;
    XMVECTOR GetPosition()         const;

private:
    float m_theta;    // azimuth  – rotation around world Y (radians)
 float m_phi;      // elevation above XZ plane (radians). This sample intentionally allows full orbit.
    float m_radius;   // distance from orbit target
    float m_focusRadius;
    XMFLOAT3 m_target;

    // Projection
    float m_fovY;
    float m_aspect;
    float m_nearZ;
    float m_farZ;

    // Mouse state
    bool m_mmbDown;
    int  m_lastMouseX;
    int  m_lastMouseY;

    void UpdateClipPlanes();
};