#pragma once

#include "Timer.h"
#include "D3D12RenderContext.h"

class D3D12Application
{
public:
    D3D12Application(UINT width, UINT height, std::wstring name);
    virtual ~D3D12Application();

	// Main entry points for the application.
    // OnInit is called once per program, while the others are called once per frame.
    virtual void OnInit() = 0;
    virtual void OnUpdate(const Timer& timer) = 0;
    virtual void OnRender(const Timer& timer) = 0;
    virtual void OnDestroy() = 0;

    // Window & Input
    virtual void OnKeyDown(UINT8 /*key*/) {}
    virtual void OnKeyUp(UINT8 /*key*/) {}
    virtual bool OnCommand(UINT /*commandId*/) { return false; }

    // Accessors
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    const WCHAR* GetTitle() const { return m_title.c_str(); }

    void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc);

protected:
    std::wstring GetAssetFullPath(LPCWSTR assetName);
    void SetCustomWindowText(LPCWSTR text);

    // Kích thước Viewport
    UINT m_width;
    UINT m_height;
    float m_aspectRatio;

    // Hệ thống Render tách biệt
    std::unique_ptr<D3D12RenderContext> m_renderContext;

    bool m_useWarpDevice;

private:
    std::wstring m_assetsPath;
    std::wstring m_title;
};