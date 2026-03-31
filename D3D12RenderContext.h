#pragma once

#include "DescriptorAllocator.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

class D3D12RenderContext
{
public:
    // FrameCount thường là 2 cho Double Buffering hoặc 3 cho Triple Buffering
    static const UINT FrameCount = 2;

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
        return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    UINT GetRtvDescriptorSize() const { return m_rtvDescriptorSize; };

    D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHeapStart() const
    {
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	}
	UINT GetDsvDescriptorSize() const { return m_dsvDescriptorSize; };
    DescriptorAllocator* GetDescriptorAllocator() const { return m_descriptorAllocator.get(); }

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
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvDescriptorSize;
    UINT m_dsvDescriptorSize;
    ComPtr<ID3D12Resource> m_depthBuffer;
    ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    std::unique_ptr<DescriptorAllocator> m_descriptorAllocator;

    // Synchronization objects
    UINT m_frameIndex;
    HANDLE m_fenceEvent;
    ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValues[FrameCount];
};