// Zenith.cpp : Defines the entry point for the application.
//

#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"

// These two exports enable the Direct3D 12 Agility SDK. They tell the loader
// which D3D12Core version to use from the app-local `D3D12` folder.
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 619; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
 // COM is needed for several Windows APIs used by the sample, including image
	// encoding through WIC when saving the viewport to disk.
	HRESULT hr = CoInitializeEx(nullptr, COINITBASE_MULTITHREADED);
	const bool coInitialized = SUCCEEDED(hr);

	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
	{
		return static_cast<int>(hr);
	}

	ZenithRenderEngine engine(1920, 1080, L"Zenith 3D");
	const int exitCode = Win32Application::Run(&engine, hInstance, nCmdShow);

	if (coInitialized)
	{
		CoUninitialize();
	}

	return exitCode;
}
