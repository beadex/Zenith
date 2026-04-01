#include "pch.h"
#include "D3D12ApplicationHelper.h"
#include "D3D12Application.h"
#include "Win32Application.h"

using namespace Microsoft::WRL;

D3D12Application::D3D12Application(UINT width, UINT height, std::wstring name) :
	m_width(width),
	m_height(height),
	m_title(name),
	m_useWarpDevice(false),
	m_aspectRatio(static_cast<float>(width) / static_cast<float>(height))
{
  // Asset lookup is based on the executable folder. That keeps shader blobs and
	// other runtime files relocatable with the built app.
	WCHAR assetsPath[512];
	GetAssetsPath(assetsPath, _countof(assetsPath));
	m_assetsPath = assetsPath;

	// Intialize the render context with the specified dimensions and title
	m_renderContext = std::make_unique<D3D12RenderContext>(m_width, m_height);
}

D3D12Application::~D3D12Application()
{
	// unique_ptr automatically cleans up the render context,
	// but we can perform any additional cleanup here if necessary.
}

_Use_decl_annotations_
void D3D12Application::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
  // The sample supports one optional switch: forcing WARP. This is useful when
	// learning D3D12 on machines without a suitable hardware adapter.
	for (int i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-warp", wcslen(argv[i])) == 0 || _wcsnicmp(argv[i], L"/warp", wcslen(argv[i])) == 0)
		{
			m_useWarpDevice = true;
			m_title = m_title + L" (WARP)";
		}
	}
	// Update the render context to use WARP if the flag was set
	if (m_renderContext) m_renderContext->SetUseWarp(m_useWarpDevice);
}

std::wstring D3D12Application::GetAssetFullPath(LPCWSTR assetName)
{
    // Most helper code works with full paths so it does not depend on the current
	// working directory of Visual Studio or the launched executable.
	return m_assetsPath + assetName;
}

void D3D12Application::SetCustomWindowText(LPCWSTR text)
{
	std::wstring windowText = m_title + L": " + text;
	SetWindowText(Win32Application::GetHwnd(), windowText.c_str());
}