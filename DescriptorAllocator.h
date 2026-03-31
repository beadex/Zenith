#pragma once

#include "D3D12ApplicationHelper.h"

using Microsoft::WRL::ComPtr;

class DescriptorHeap
{
public:
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
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_heap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(UINT index) const
	{
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
		m_device = device;
		m_staticHeap.Initialize(device);
		for (auto& dynamicHeap : m_dynamicHeaps)
		{
			dynamicHeap.Initialize(device);
		}
	}

	void ResetStaticDescriptors()
	{
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
	explicit RenderTargetAllocator(UINT descriptorCapacity)
		: DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, descriptorCapacity)
	{
	}
};

class DepthStencilAllocator : public DescriptorHeap
{
public:
	explicit DepthStencilAllocator(UINT descriptorCapacity)
		: DescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, descriptorCapacity)
	{
	}
};
