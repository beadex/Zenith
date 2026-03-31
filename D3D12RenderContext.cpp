#include "pch.h"
#include "D3D12RenderContext.h"
#include "D3D12ApplicationHelper.h"

#pragma comment(lib, "Windowscodecs.lib")

namespace
{
	bool SaveBufferAsPng(
		ID3D12Resource* readbackBuffer,
		const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
		UINT width,
		UINT height,
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
			   DXGI_FORMAT_R8G8B8A8_UNORM,
			   footprint.Footprint.RowPitch,
			   static_cast<SIZE_T>(footprint.Footprint.RowPitch) * height,
			   mappedData
		};

		const HRESULT hr = DirectX::SaveToWICFile(
			image,
			DirectX::WIC_FLAGS_NONE,
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

	CreateDevice(useWarp);
	CreateResources(hwnd);
}

void D3D12RenderContext::BeginUpload()
{
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));
}

void D3D12RenderContext::EndUpload()
{
	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	WaitForGpu(); // Stalls CPU until GPU finishes the copy
}

void D3D12RenderContext::CreateDevice(bool useWarp)
{
	// Enable the D3D12 debug layer if in debug mode
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
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(m_factory.Get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
		));
	}

	// Create command queue
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
}

void D3D12RenderContext::CreateResources(HWND hwnd)
{
	const DXGI_SAMPLE_DESC sampleDesc = { 1, 0 };
	// Create swap chain
	{
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.BufferCount = FrameCount;
		swapChainDesc.Width = m_width;
		swapChainDesc.Height = m_height;
		swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

	// Create descriptor heap for render target views (RTVs)
	{
        m_rtvHeap = std::make_unique<RenderTargetAllocator>(FrameCount);
		m_rtvHeap->Initialize(m_device.Get());
		m_rtvDescriptorSize = m_rtvHeap->GetDescriptorSize();

		m_dsvHeap = std::make_unique<DepthStencilAllocator>(1);
		m_dsvHeap->Initialize(m_device.Get());

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

        m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsvHeap->GetCpuHandle(0));

		m_dsvDescriptorSize = m_dsvHeap->GetDescriptorSize();

      m_descriptorAllocator = std::make_unique<CbvSrvUavAllocator>(FrameCount);
		m_descriptorAllocator->Initialize(m_device.Get());
	}

	// Create render target views (RTVs) for each frame in the swap chain
	{
		for (UINT n = 0; n < FrameCount; n++)
		{
			ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
         m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, m_rtvHeap->GetCpuHandle(n));

			ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[n])));
		}
	}

	// Create command allocator and command list
	{

		ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
		m_commandList->Close();
	}

	// Create synchronization objects (fence and event)
	{
		ThrowIfFailed(m_device->CreateFence(m_fenceValues[m_frameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValues[m_frameIndex]++;

		// Create an event handle to use for frame synchronization
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}
}

void D3D12RenderContext::Prepare()
{
	// Reset command allocator and command list for the current frame
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());
	ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

	// Transition from PRESENT to RENDER_TARGET state for the current back buffer
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList->ResourceBarrier(1, &barrier);
	m_descriptorAllocator->BeginFrame(m_frameIndex);
}

bool D3D12RenderContext::Present(const std::wstring& capturePath)
{
	const bool captureRequested = !capturePath.empty();
	ComPtr<ID3D12Resource> readbackBuffer;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};

	if (captureRequested)
	{
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

	// Transition from RENDER_TARGET to PRESENT state for the current back buffer
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &barrier);

	// Close the command list and execute it to render the frame
	ThrowIfFailed(m_commandList->Close());

	// Execute the command list
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
			capturePath);
	}

	// Signal and increment the fence value for the current frame
	MoveToNextFrame();
	return captureSucceeded;
}

// Prepare to render the next frame.
void D3D12RenderContext::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const UINT64 currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
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
	// Schedule a Signal command in the queue
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

			// Check to see whether the adapter supports Direct3D 12, but don't create the actual device yet.
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
