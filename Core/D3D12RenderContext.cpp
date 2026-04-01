#include "pch.h"
#include "D3D12RenderContext.h"
#include "D3D12ApplicationHelper.h"

#pragma comment(lib, "Windowscodecs.lib")

// ---------------------------------------------------------------------------
// D3D12RenderContext
//
// This file contains the low-level Direct3D 12 setup and frame plumbing:
//   - device creation
//   - swap chain creation
//   - command queue / command list / command allocator setup
//   - RTV / DSV creation
//   - fence synchronization
//   - frame begin / present transitions
//
// ZenithRenderEngine sits above this layer and uses it to record actual scene
// rendering commands.
// ---------------------------------------------------------------------------

namespace
{
	// Helper used when the user chooses "Render Image".
	 // The back buffer is copied into a readback buffer, then encoded as PNG.
	bool SaveBufferAsPng(
		ID3D12Resource* readbackBuffer,
		const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
		UINT width,
		UINT height,
		DXGI_FORMAT backBufferFormat,
		const std::wstring& filePath)
	{
		BYTE* mappedData = nullptr;
		const CD3DX12_RANGE readRange(0, static_cast<SIZE_T>(footprint.Footprint.RowPitch) * height);
		if (FAILED(readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData))))
		{
			return false;
		}

		const DirectX::Image image = {
			   width,
			   height,
			   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			   footprint.Footprint.RowPitch,
			   static_cast<SIZE_T>(footprint.Footprint.RowPitch) * height,
			   mappedData
		};

		const HRESULT hr = DirectX::SaveToWICFile(
			image,
			DirectX::WIC_FLAGS_FORCE_SRGB,
			GetWICCodec(WIC_CODEC_PNG),
			filePath.c_str());

		readbackBuffer->Unmap(0, nullptr);
		return SUCCEEDED(hr);
	}
}

D3D12RenderContext::D3D12RenderContext(UINT width, UINT height) :
	m_width(width),
	m_height(height),
	m_useWarp(false),
	m_frameIndex(0),
	m_rtvDescriptorSize(0),
	m_dsvDescriptorSize(0),
	m_fenceValues{},
	m_fenceEvent(nullptr)
{
}

D3D12RenderContext::~D3D12RenderContext()
{
	WaitForGpu();
	CloseHandle(m_fenceEvent);
}

void D3D12RenderContext::Initialize(HWND hwnd, bool useWarp)
{
	m_useWarp = useWarp;

	// Device creation and resource creation are split mainly to keep the code
	  // easier to read: first create the GPU device/queue, then create the swap
	  // chain, descriptor heaps, command list, fences, and related resources.
	CreateDevice(useWarp);
	CreateResources(hwnd);
}

void D3D12RenderContext::BeginUpload()
{
	// Upload work reuses the same direct command list in this sample. The code is
	  // intentionally simple: open list, record copy commands, execute, then wait.
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));
}

void D3D12RenderContext::EndUpload()
{
	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	// This is conservative but beginner-friendly: we wait so the caller knows the
	   // upload is finished before freeing staging resources.
	WaitForGpu();
}

void D3D12RenderContext::CreateDevice(bool useWarp)
{
	// The debug layer is one of the most useful learning tools in D3D12. It helps
	// catch incorrect state transitions, binding mistakes, and resource misuse.
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));

	if (m_useWarp)
	{
		// WARP is Microsoft's software rasterizer. It is much slower than hardware
		 // but useful when no physical GPU is available or for debugging.
		ComPtr<IDXGIAdapter> warpAdapter;
		ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			warpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
		));
	}
	else
	{
		// In the normal path, pick a hardware adapter that supports D3D12.
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(m_factory.Get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
		));
	}

	// The command queue is where recorded command lists are submitted for GPU
	   // execution. This sample uses a direct queue because it performs graphics work.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = m_backBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(m_device->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)));

	m_4xMsaaQuality = msQualityLevels.NumQualityLevels;
	assert(m_4xMsaaQuality > 0 && "Unexpected MSAA quality level.");
}

void D3D12RenderContext::CreateResources(HWND hwnd)
{
	const DXGI_SAMPLE_DESC sampleDesc = { m_4xMsaaQuality, m_4xMsaaQuality - 1 };
	// The swap chain owns the back buffers that are presented to the window.
	// Double buffering is used here via FrameCount = 2.
	{
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.BufferCount = FrameCount;
		swapChainDesc.Width = m_width;
		swapChainDesc.Height = m_height;
		swapChainDesc.Format = m_backBufferFormat;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.SampleDesc = sampleDesc;

		ComPtr<IDXGISwapChain1> swapChain;
		ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
			m_commandQueue.Get(),
			hwnd,
			&swapChainDesc,
			nullptr,
			nullptr,
			&swapChain
		));

		ThrowIfFailed(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

		ThrowIfFailed(swapChain.As(&m_swapChain));
		m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
	}

	// Create the descriptor manager and depth buffer resources.
	// RTV/DSV descriptors are CPU-only descriptors used by OMSetRenderTargets,
	// while the CBV/SRV/UAV allocator is used for shader-visible binding.
	{
		m_descriptorManager = std::make_unique<DescriptorManager>(FrameCount);
		m_descriptorManager->Initialize(m_device.Get());
		auto* rtvAllocator = m_descriptorManager->GetRtvAllocator();
		auto* dsvAllocator = m_descriptorManager->GetDsvAllocator();
		auto* cbvSrvUavAllocator = m_descriptorManager->GetCbvSrvUavAllocator();

		// The depth buffer is a normal GPU texture resource. The DSV descriptor is
		   // just the view that lets the pipeline treat that texture as a depth target.
		D3D12_RESOURCE_DESC depthDesc = {};
		depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depthDesc.Width = m_width;
		depthDesc.Height = m_height;
		depthDesc.DepthOrArraySize = 1;
		depthDesc.MipLevels = 1;
		depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.SampleDesc = sampleDesc;
		depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE depthClear = {};
		depthClear.Format = DXGI_FORMAT_D32_FLOAT;
		depthClear.DepthStencil.Depth = 1.0f;

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&depthDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&depthClear,
			IID_PPV_ARGS(&m_depthBuffer)
		));

		m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, dsvAllocator->GetCpuHandle(0));

		D3D12_RESOURCE_DESC shadowDesc = {};
		shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		shadowDesc.Width = ShadowMapWidth;
		shadowDesc.Height = ShadowMapHeight;
		shadowDesc.DepthOrArraySize = 1;
		shadowDesc.MipLevels = 1;
		shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		shadowDesc.SampleDesc = { 1, 0 };
		shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE shadowClear = {};
		shadowClear.Format = DXGI_FORMAT_D32_FLOAT;
		shadowClear.DepthStencil.Depth = 1.0f;

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&shadowDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			&shadowClear,
			IID_PPV_ARGS(&m_shadowMap)
		));

		D3D12_DEPTH_STENCIL_VIEW_DESC shadowDsvDesc = {};
		shadowDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		shadowDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		shadowDsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		m_device->CreateDepthStencilView(m_shadowMap.Get(), &shadowDsvDesc, dsvAllocator->GetCpuHandle(1));

		m_shadowMapSrvIndex = cbvSrvUavAllocator->AllocateStaticDescriptor();

		D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
		shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		shadowSrvDesc.Texture2D.MipLevels = 1;
		m_device->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, cbvSrvUavAllocator->GetStaticCpuHandle(m_shadowMapSrvIndex));

		for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
		{
            D3D12_RESOURCE_DESC pointShadowDesc = shadowDesc;
			pointShadowDesc.Width = PointShadowMapWidth;
			pointShadowDesc.Height = PointShadowMapHeight;

			ThrowIfFailed(m_device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
                &pointShadowDesc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				&shadowClear,
				IID_PPV_ARGS(&m_pointShadowMaps[faceIndex])
			));

			m_device->CreateDepthStencilView(m_pointShadowMaps[faceIndex].Get(), &shadowDsvDesc, dsvAllocator->GetCpuHandle(2 + faceIndex));

			m_pointShadowMapSrvIndices[faceIndex] = cbvSrvUavAllocator->AllocateStaticDescriptor();
			m_device->CreateShaderResourceView(m_pointShadowMaps[faceIndex].Get(), &shadowSrvDesc, cbvSrvUavAllocator->GetStaticCpuHandle(m_pointShadowMapSrvIndices[faceIndex]));
		}

		m_rtvDescriptorSize = rtvAllocator->GetDescriptorSize();
		m_dsvDescriptorSize = dsvAllocator->GetDescriptorSize();
	}

	// Each swap-chain back buffer gets its own RTV descriptor.
	{
		auto* rtvAllocator = m_descriptorManager->GetRtvAllocator();
		for (UINT n = 0; n < FrameCount; n++)
		{
			ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
			m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvAllocator->GetCpuHandle(n));

			ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[n])));
		}
	}

	// Create the main graphics command list. It starts in the open state, so we
	// close it immediately because the frame loop expects to Reset() it later.
	{

		ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
		m_commandList->Close();
	}

	// Fences are how the CPU knows whether the GPU has finished using resources
	   // like command allocators or back buffers.
	{
		ThrowIfFailed(m_device->CreateFence(m_fenceValues[m_frameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValues[m_frameIndex]++;

		// Create an event handle to use for frame synchronization
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait once so initialization is fully complete before the app continues.
		WaitForGpu();
	}
}

void D3D12RenderContext::Prepare()
{
	// Reset the allocator and command list so a fresh set of commands can be
	 // recorded for the current frame.
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

	// D3D12 requires explicit state transitions. A back buffer cannot be rendered
	 // to while still in PRESENT state.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList->ResourceBarrier(1, &barrier);

	// BeginFrame() copies all long-lived descriptors (for example texture SRVs)
	// into the current shader-visible heap, then resets the dynamic allocation
	// cursor so the renderer can append per-frame descriptors after them.
	m_descriptorManager->GetCbvSrvUavAllocator()->BeginFrame(m_frameIndex);
}

bool D3D12RenderContext::Present(const std::wstring& capturePath)
{
	const bool captureRequested = !capturePath.empty();
	ComPtr<ID3D12Resource> readbackBuffer;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};

	if (captureRequested)
	{
		// To save an image, the render target is copied into a READBACK buffer that
		  // the CPU can map. The back buffer itself is not directly CPU-readable.
		const D3D12_RESOURCE_DESC renderTargetDesc = m_renderTargets[m_frameIndex]->GetDesc();
		UINT64 readbackBufferSize = 0;
		m_device->GetCopyableFootprints(&renderTargetDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBufferSize);

		ThrowIfFailed(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(readbackBufferSize),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&readbackBuffer)));

		auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[m_frameIndex].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_commandList->ResourceBarrier(1, &toCopySource);

		D3D12_TEXTURE_COPY_LOCATION destination = {};
		destination.pResource = readbackBuffer.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		destination.PlacedFootprint = footprint;

		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = m_renderTargets[m_frameIndex].Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		source.SubresourceIndex = 0;

		m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

		auto backToRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderTargets[m_frameIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_commandList->ResourceBarrier(1, &backToRenderTarget);
	}

	// After rendering, the back buffer must be returned to PRESENT state before
	 // the swap chain can display it.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &barrier);

	// Close the command list and execute it to render the frame
	ThrowIfFailed(m_commandList->Close());

	// Submit the finished command list to the queue.
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Present the frame
	ThrowIfFailed(m_swapChain->Present(1, 0));

	bool captureSucceeded = true;
	if (captureRequested)
	{
		WaitForGpu();
		captureSucceeded = SaveBufferAsPng(
			readbackBuffer.Get(),
			footprint,
			m_width,
			m_height,
			m_backBufferFormat,
			capturePath);
	}

	// Advance to the next frame and synchronize allocator/back-buffer reuse.
	MoveToNextFrame();
	return captureSucceeded;
}

// Prepare to render the next frame.
void D3D12RenderContext::MoveToNextFrame()
{
	// Signal the fence value associated with the work just submitted.
	const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame's allocator/back buffer is still in use by the GPU, wait.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

// Wait for pending GPU work to complete.
void D3D12RenderContext::WaitForGpu()
{
	// Full GPU flush. This is simple and safe, though not something a high-end
	 // renderer would want to do every frame.
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));

	// Wait until the fence has been processed.
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

void D3D12RenderContext::GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter, bool requestHighPerformanceAdapter)
{
	*ppAdapter = nullptr;

	ComPtr<IDXGIAdapter1> adapter;

	ComPtr<IDXGIFactory6> factory6;

	if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
	{
		// Prefer a modern DXGI path that can ask for high-performance adapters.
		for (
			UINT adapterIndex = 0;
			SUCCEEDED(factory6->EnumAdapterByGpuPreference(
				adapterIndex,
				requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED,
				IID_PPV_ARGS(&adapter)
			));
			++adapterIndex
			)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				// Don't select the Basic Render Driver adapter
				// If you specifically want a software adapter, pass in "/warp" on the command line

				continue;
			}

			// Probe whether the adapter supports D3D12 without creating the final device yet.
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
			{
				break;
			}
		}
	}

	if (adapter.Get() == nullptr)
	{
		for (UINT adapterIndex = 0; SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, &adapter)); ++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				// Don't select the Basic Render Driver adapter
				// If you specifically want a software adapter, pass in "/warp" on the command line

				continue;
			}

			// Check to see whether the adapter supports Direct3D 12, but don't create the actual device yet.
			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
			{
				break;
			}
		}
	}

	*ppAdapter = adapter.Detach();
}
