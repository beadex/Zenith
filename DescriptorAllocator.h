#pragma once

#include "D3D12ApplicationHelper.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Descriptor system overview
//
// D3D12 does not let shaders access textures / constant buffers directly.
// Instead, the GPU reads small records called descriptors from descriptor heaps.
//
// This file builds the descriptor system in layers:
//   DescriptorHeap        -> minimal wrapper over one ID3D12DescriptorHeap
//   CbvSrvUavAllocator    -> static + per-frame dynamic CBV/SRV/UAV management
//   RenderTargetAllocator -> typed RTV heap wrapper
//   DepthStencilAllocator -> typed DSV heap wrapper
//   DescriptorManager     -> groups all allocators in one owner
// ---------------------------------------------------------------------------

class DescriptorHeap
{
public:
	// The base heap wrapper only knows how to create a heap and compute handles.
	   // It does not implement allocation policy by itself.
	DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE heapType, D3D12_DESCRIPTOR_HEAP_FLAGS heapFlags, UINT descriptorCapacity)
		: m_device(nullptr)
		, m_heapType(heapType)
		, m_heapFlags(heapFlags)
		, m_descriptorCapacity(descriptorCapacity)
		, m_descriptorSize(0)
	{
	}

	void Initialize(ID3D12Device* device)
	{
		// Different heap types (RTV, DSV, CBV/SRV/UAV) use different descriptor
		  // sizes, so the increment size must be queried from the device.
		m_device = device;
		m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(m_heapType);

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.NumDescriptors = m_descriptorCapacity;
		heapDesc.Type = m_heapType;
		heapDesc.Flags = m_heapFlags;
		ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap)));
	}

	ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
	UINT GetDescriptorSize() const { return m_descriptorSize; }
	UINT GetDescriptorCapacity() const { return m_descriptorCapacity; }
	bool IsShaderVisible() const { return (m_heapFlags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0; }

	D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(UINT index) const
	{
		// CPU descriptor handles are just pointer-like values plus an index-based
		  // offset. The descriptor size tells us how far to move for each slot.
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(UINT index) const
	{
		// Only shader-visible heaps expose GPU handles. RTV and DSV heaps are not
		   // read by shaders, so asking them for GPU handles is an error.
		if (!IsShaderVisible())
		{
			throw std::runtime_error("Descriptor heap is not shader visible");
		}

		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
		return handle;
	}

	D3D12_DESCRIPTOR_HEAP_TYPE GetHeapType() const { return m_heapType; }

protected:
	ID3D12Device* m_device;
	D3D12_DESCRIPTOR_HEAP_TYPE m_heapType;
	D3D12_DESCRIPTOR_HEAP_FLAGS m_heapFlags;
	UINT m_descriptorCapacity;
	UINT m_descriptorSize;
	ComPtr<ID3D12DescriptorHeap> m_heap;
};

class CbvSrvUavAllocator
{
public:
	// The CBV/SRV/UAV path is more advanced than RTV/DSV because shaders need a
	  // shader-visible heap every frame. This allocator therefore uses two layers:
	  //   1. a static non-shader-visible heap for long-lived descriptors
	  //   2. a per-frame shader-visible heap used during rendering
	static constexpr UINT MaxStaticDescriptors = 1024;
	static constexpr UINT MaxDynamicDescriptorsPerFrame = 64;
	static constexpr UINT MaxShaderVisibleDescriptors = MaxStaticDescriptors + MaxDynamicDescriptorsPerFrame;

	explicit CbvSrvUavAllocator(UINT frameCount)
		: m_device(nullptr)
		, m_frameCount(frameCount)
		, m_currentFrameIndex(0)
		, m_staticDescriptorCount(0)
		, m_staticDescriptorAllocated(MaxStaticDescriptors, false)
		, m_staticHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, MaxStaticDescriptors)
	{
		m_dynamicDescriptorCounts.resize(frameCount, 0);
		m_dynamicHeaps.reserve(frameCount);
		for (UINT frame = 0; frame < frameCount; ++frame)
		{
			m_dynamicHeaps.emplace_back(
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
				MaxShaderVisibleDescriptors);
		}
	}

	void Initialize(ID3D12Device* device)
	{
		// Static descriptors are written once and copied later.
		  // Dynamic heaps are the per-frame heaps that are actually bound to shaders.
		m_device = device;
		m_staticHeap.Initialize(device);
		for (auto& dynamicHeap : m_dynamicHeaps)
		{
			dynamicHeap.Initialize(device);
		}
	}

	void ResetStaticDescriptors()
	{
		// Full reset for the long-lived allocation state. This is rarely needed in
		// the current sample, but useful if the whole descriptor pool is rebuilt.
		m_staticDescriptorCount = 0;
		std::fill(m_staticDescriptorAllocated.begin(), m_staticDescriptorAllocated.end(), false);
		m_freeStaticDescriptors.clear();
		for (UINT frame = 0; frame < m_frameCount; ++frame)
		{
			m_dynamicDescriptorCounts[frame] = 0;
		}
	}

	void BeginFrame(UINT frameIndex)
	{
		// At frame begin, all currently-live static descriptors are copied into the
		 // current shader-visible heap. After that, dynamic per-frame descriptors are
		 // allocated after the copied static range.
		m_currentFrameIndex = frameIndex;
		m_dynamicDescriptorCounts[m_currentFrameIndex] = m_staticDescriptorCount;
		if (m_staticDescriptorCount == 0)
		{
			return;
		}

		m_device->CopyDescriptorsSimple(
			m_staticDescriptorCount,
			m_dynamicHeaps[m_currentFrameIndex].GetCpuHandle(0),
			m_staticHeap.GetCpuHandle(0),
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	UINT AllocateStaticDescriptor()
	{
		// Reuse a freed slot if possible. This prevents descriptor exhaustion when
		 // models are loaded and destroyed repeatedly.
		if (!m_freeStaticDescriptors.empty())
		{
			const UINT slot = m_freeStaticDescriptors.back();
			m_freeStaticDescriptors.pop_back();
			m_staticDescriptorAllocated[slot] = true;
			m_staticDescriptorCount = (std::max)(m_staticDescriptorCount, slot + 1);
			return slot;
		}

		if (m_staticDescriptorCount >= MaxStaticDescriptors)
		{
			throw std::runtime_error("Static descriptor heap capacity exceeded");
		}

		const UINT slot = m_staticDescriptorCount++;
		m_staticDescriptorAllocated[slot] = true;
		return slot;
	}

	void ReleaseStaticDescriptor(UINT index)
	{
		// Released slots go onto a free list. If the released slot happens to be at
		// the logical end of the active range, the active count is trimmed so future
		// BeginFrame() copies do not include dead tail descriptors.
		if (index >= m_staticDescriptorCount || !m_staticDescriptorAllocated[index])
		{
			return;
		}

		m_staticDescriptorAllocated[index] = false;
		m_freeStaticDescriptors.push_back(index);

		while (m_staticDescriptorCount > 0 && !m_staticDescriptorAllocated[m_staticDescriptorCount - 1])
		{
			const UINT releasedTailIndex = m_staticDescriptorCount - 1;
			--m_staticDescriptorCount;

			auto freeSlot = std::find(m_freeStaticDescriptors.begin(), m_freeStaticDescriptors.end(), releasedTailIndex);
			if (freeSlot != m_freeStaticDescriptors.end())
			{
				m_freeStaticDescriptors.erase(freeSlot);
			}
		}
	}

	UINT AllocateDynamicDescriptor()
	{
		// Dynamic descriptors are short-lived and only valid for the current frame.
		  // They are typically used for temporary CBVs like SceneData or LightingData.
		UINT& dynamicDescriptorCount = m_dynamicDescriptorCounts[m_currentFrameIndex];
		if (dynamicDescriptorCount >= MaxShaderVisibleDescriptors)
		{
			throw std::runtime_error("Dynamic descriptor heap capacity exceeded");
		}

		return dynamicDescriptorCount++;
	}

	UINT GetDescriptorSize() const { return m_staticHeap.GetDescriptorSize(); }
	UINT GetStaticDescriptorCount() const { return m_staticDescriptorCount; }
	ID3D12DescriptorHeap* GetShaderVisibleHeap() const { return m_dynamicHeaps[m_currentFrameIndex].GetHeap(); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetStaticCpuHandle(UINT index) const { return m_staticHeap.GetCpuHandle(index); }
	D3D12_CPU_DESCRIPTOR_HANDLE GetDynamicCpuHandle(UINT index) const { return m_dynamicHeaps[m_currentFrameIndex].GetCpuHandle(index); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetDynamicGpuHandle(UINT index) const { return m_dynamicHeaps[m_currentFrameIndex].GetGpuHandle(index); }

private:
	ID3D12Device* m_device;
	UINT m_frameCount;
	UINT m_currentFrameIndex;
	UINT m_staticDescriptorCount;
	std::vector<bool> m_staticDescriptorAllocated;
	std::vector<UINT> m_freeStaticDescriptors;
	std::vector<UINT> m_dynamicDescriptorCounts;
	DescriptorHeap m_staticHeap;
	std::vector<DescriptorHeap> m_dynamicHeaps;
};

class RenderTargetAllocator : public DescriptorHeap
{
public:
	// Thin type wrapper for RTV heaps. No shader-visible behavior is needed.
	explicit RenderTargetAllocator(UINT descriptorCapacity)
		: DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, descriptorCapacity)
	{
	}
};

class DepthStencilAllocator : public DescriptorHeap
{
public:
	// Thin type wrapper for DSV heaps.
	explicit DepthStencilAllocator(UINT descriptorCapacity)
		: DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, descriptorCapacity)
	{
	}
};

class DescriptorManager
{
public:
	// Convenience owner that keeps all descriptor allocators together so the
	   // render context does not need to manage them as separate members.
	explicit DescriptorManager(UINT frameCount)
		: m_cbvSrvUavAllocator(frameCount)
		, m_rtvAllocator(frameCount)
		, m_dsvAllocator(1)
	{
	}

	void Initialize(ID3D12Device* device)
	{
		m_cbvSrvUavAllocator.Initialize(device);
		m_rtvAllocator.Initialize(device);
		m_dsvAllocator.Initialize(device);
	}

	CbvSrvUavAllocator* GetCbvSrvUavAllocator() { return &m_cbvSrvUavAllocator; }
	const CbvSrvUavAllocator* GetCbvSrvUavAllocator() const { return &m_cbvSrvUavAllocator; }
	RenderTargetAllocator* GetRtvAllocator() { return &m_rtvAllocator; }
	const RenderTargetAllocator* GetRtvAllocator() const { return &m_rtvAllocator; }
	DepthStencilAllocator* GetDsvAllocator() { return &m_dsvAllocator; }
	const DepthStencilAllocator* GetDsvAllocator() const { return &m_dsvAllocator; }

private:
	CbvSrvUavAllocator m_cbvSrvUavAllocator;
	RenderTargetAllocator m_rtvAllocator;
	DepthStencilAllocator m_dsvAllocator;
};
