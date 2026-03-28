#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"

ZenithRenderEngine::ZenithRenderEngine(UINT width, UINT height, std::wstring name) :
    D3D12Application(width, height, name)
{
}

void ZenithRenderEngine::OnInit()
{
	// 1. Initialize D3D12 Context (Device, Swap Chain, Command Queue, RTV Heap...)
    m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

    // 2. Load Assets (Shader, Geometry...)
    LoadPipeline();
    LoadAssets();
}

void ZenithRenderEngine::LoadPipeline()
{
	// Here we will create Root Signature, Pipeline State Object (PSO), and any other pipeline-related resources.
}

void ZenithRenderEngine::LoadAssets()
{
	// Here we will load shaders, create vertex buffers, index buffers, constant buffers, and any other assets needed for rendering.
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	// Update engine logic, animations, camera, etc. based on the elapsed time (timer).
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	// 1. Prepare the command list for rendering (Reset command allocator, command list, set render target, etc.)
    m_renderContext->Prepare();

    auto commandList = m_renderContext->GetCommandList();

	// 2. Record commands to clear the render target and set it for drawing. You can also set the viewport and scissor rect here if needed.
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        m_renderContext->GetRtvHeapStart(), // Bạn cần thêm Getter này trong RenderContext
        m_renderContext->GetCurrentFrameIndex(),
        m_renderContext->GetRtvDescriptorSize()
    );

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

	// 3. Record commands to draw geometry here (Set pipeline state, set root signature, set vertex/index buffers, draw calls, etc.)
    m_renderContext->Present();
}

void ZenithRenderEngine::OnDestroy()
{
	// Make sure to wait for the GPU to finish all pending work before exiting to avoid access violations and ensure proper cleanup of resources.
    m_renderContext->WaitForGpu();
}