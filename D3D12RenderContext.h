#pragma once

#include "DescriptorAllocator.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class D3D12RenderContext
{
public:
	// FrameCount thường là 2 cho Double Buffering hoặc 3 cho Triple Buffering
	static const UINT FrameCount = 2;
    // The first shadow-mapping step uses one fixed-size depth texture.
	//
	// Bigger values -> sharper shadows but more GPU cost.
	// Smaller values -> blurrier / more jagged shadows but cheaper.
	static const UINT ShadowMapWidth = 2048;
	static const UINT ShadowMapHeight = 2048;

	D3D12RenderContext(UINT width, UINT height);
	~D3D12RenderContext();

	// Intialize the D3D12 context with the given window handle and whether to use WARP (software rendering)
	void Initialize(HWND hwnd, bool useWarp);

	// Begin the upload process for resources (Reset command allocator, command list, etc.)
	void BeginUpload();

	// End the upload process and execute the command list to upload resources to the GPU (Close command list, execute, wait for GPU, etc.)
	void EndUpload();

	// Prepare command list for rendering (Reset command allocator, command list, set render target, etc.)
	void Prepare();

	// Finish rendering and present the frame (Execute command list, present swap chain, signal fence, etc.)
	bool Present(const std::wstring& capturePath = L"");

	// Wait for GPU to finish processing the current frame (Wait for fence, reset command allocator, etc.)
	void WaitForGpu();

	// Move to the next frame in the swap chain (Signal fence, update frame index, wait for next frame, etc.)
	void MoveToNextFrame();

	// Setters & Getters
	void SetUseWarp(bool useWarp) { m_useWarp = useWarp; }

	ID3D12Device* GetDevice() const { return m_device.Get(); }
	ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }
	UINT GetCurrentFrameIndex() const { return m_frameIndex; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHeapStart() const
	{
		return m_descriptorManager->GetRtvAllocator()->GetCpuHandle(0);
	}
	UINT GetRtvDescriptorSize() const { return m_rtvDescriptorSize; };

	D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHeapStart() const
	{
		return m_descriptorManager->GetDsvAllocator()->GetCpuHandle(0);
	}
	UINT GetDsvDescriptorSize() const { return m_dsvDescriptorSize; };
 // Slot 0 in the DSV heap is the normal camera depth buffer used by the main pass.
	D3D12_CPU_DESCRIPTOR_HANDLE GetMainDepthDsv() const { return m_descriptorManager->GetDsvAllocator()->GetCpuHandle(0); }
 // Slot 1 in the DSV heap is the shadow map depth buffer used by the light pass.
	D3D12_CPU_DESCRIPTOR_HANDLE GetShadowMapDsv() const { return m_descriptorManager->GetDsvAllocator()->GetCpuHandle(1); }
	CbvSrvUavAllocator* GetCbvSrvUavAllocator() const { return m_descriptorManager->GetCbvSrvUavAllocator(); }
	RenderTargetAllocator* GetRtvAllocator() const { return m_descriptorManager->GetRtvAllocator(); }
	DepthStencilAllocator* GetDsvAllocator() const { return m_descriptorManager->GetDsvAllocator(); }
	UINT Get4xMsaaQuality() const { return m_4xMsaaQuality; }
  // The shadow map is rendered as depth, then sampled later like a texture.
	ID3D12Resource* GetShadowMap() const { return m_shadowMap.Get(); }
   // This is the SRV slot index copied into the shader-visible heap each frame.
	UINT GetShadowMapSrvIndex() const { return m_shadowMapSrvIndex; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetShadowMapSrvCpuHandle() const { return m_descriptorManager->GetCbvSrvUavAllocator()->GetStaticCpuHandle(m_shadowMapSrvIndex); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSrvGpuHandle() const { return m_descriptorManager->GetCbvSrvUavAllocator()->GetDynamicGpuHandle(m_shadowMapSrvIndex); }
   // Shadow rendering uses its own viewport/scissor because the shadow map size
	// is independent from the window size.
	const CD3DX12_VIEWPORT& GetShadowMapViewport() const { return m_shadowViewport; }
	const CD3DX12_RECT& GetShadowMapScissorRect() const { return m_shadowScissorRect; }

private:
	ComPtr<IDXGIFactory4> m_factory;

	void CreateDevice(bool useWarp);
	void CreateResources(HWND hwnd);
	void GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter, bool requestHighPerformanceAdapter = false);

	// Kích thước buffer
	UINT m_width;
	UINT m_height;
	bool m_useWarp;

	// Pipeline objects
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<IDXGISwapChain3> m_swapChain;
	UINT m_rtvDescriptorSize;
	UINT m_dsvDescriptorSize;
	ComPtr<ID3D12Resource> m_depthBuffer;
 // Dedicated depth texture that stores the scene as seen from the light.
	// The main pass later samples this to decide whether a pixel is shadowed.
	ComPtr<ID3D12Resource> m_shadowMap;
	ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
	ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
	ComPtr<ID3D12GraphicsCommandList> m_commandList;
	std::unique_ptr<DescriptorManager> m_descriptorManager;
	UINT m_4xMsaaQuality = 0;
	DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Static SRV slot for the shadow map in the descriptor allocator.
	UINT m_shadowMapSrvIndex = UINT_MAX;
  // These describe the rasterization area when rendering from the light.
	CD3DX12_VIEWPORT m_shadowViewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(ShadowMapWidth), static_cast<float>(ShadowMapHeight));
	CD3DX12_RECT m_shadowScissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(ShadowMapWidth), static_cast<LONG>(ShadowMapHeight));

	// Synchronization objects
	UINT m_frameIndex;
	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValues[FrameCount];
};