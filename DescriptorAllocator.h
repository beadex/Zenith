#pragma once

#include "D3D12ApplicationHelper.h"

using Microsoft::WRL::ComPtr;

class DescriptorAllocator
{
public:
	static constexpr UINT MaxStaticDescriptors = 1024;
	static constexpr UINT MaxDynamicDescriptorsPerFrame = 64;
	static constexpr UINT MaxShaderVisibleDescriptors = MaxStaticDescriptors + MaxDynamicDescriptorsPerFrame;

	explicit DescriptorAllocator(UINT frameCount)
		: m_device(nullptr)
		, m_frameCount(frameCount)
		, m_currentFrameIndex(0)
		, m_descriptorSize(0)
		, m_staticDescriptorCount(0)
		, m_dynamicDescriptorCounts(frameCount, 0)
		, m_dynamicHeaps(frameCount)
	{
	}

	void Initialize(ID3D12Device* device)
	{
		m_device = device;
		m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_DESCRIPTOR_HEAP_DESC staticHeapDesc = {};
		staticHeapDesc.NumDescriptors = MaxStaticDescriptors;
		staticHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		staticHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(m_device->CreateDescriptorHeap(&staticHeapDesc, IID_PPV_ARGS(&m_staticHeap)));

		D3D12_DESCRIPTOR_HEAP_DESC dynamicHeapDesc = {};
		dynamicHeapDesc.NumDescriptors = MaxShaderVisibleDescriptors;
		dynamicHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		dynamicHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		for (UINT frame = 0; frame < m_frameCount; ++frame)
		{
			ThrowIfFailed(m_device->CreateDescriptorHeap(&dynamicHeapDesc, IID_PPV_ARGS(&m_dynamicHeaps[frame])));
		}
	}

	void ResetStaticDescriptors()
	{
		m_staticDescriptorCount = 0;
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
			m_dynamicHeaps[m_currentFrameIndex]->GetCPUDescriptorHandleForHeapStart(),
			m_staticHeap->GetCPUDescriptorHandleForHeapStart(),
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	UINT AllocateStaticDescriptor()
	{
		if (m_staticDescriptorCount >= MaxStaticDescriptors)
		{
			throw std::runtime_error("Static descriptor heap capacity exceeded");
		}

		return m_staticDescriptorCount++;
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

	UINT GetDescriptorSize() const { return m_descriptorSize; }
	UINT GetStaticDescriptorCount() const { return m_staticDescriptorCount; }
	ID3D12DescriptorHeap* GetShaderVisibleHeap() const
	{
		return m_dynamicHeaps[m_currentFrameIndex].Get();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE GetStaticCpuHandle(UINT index) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_staticHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
		return handle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE GetDynamicCpuHandle(UINT index) const
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = m_dynamicHeaps[m_currentFrameIndex]->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE GetDynamicGpuHandle(UINT index) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE handle = m_dynamicHeaps[m_currentFrameIndex]->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
		return handle;
	}

private:
	ID3D12Device* m_device;
	UINT m_frameCount;
	UINT m_currentFrameIndex;
	UINT m_descriptorSize;
	UINT m_staticDescriptorCount;
	std::vector<UINT> m_dynamicDescriptorCounts;
	ComPtr<ID3D12DescriptorHeap> m_staticHeap;
	std::vector<ComPtr<ID3D12DescriptorHeap>> m_dynamicHeaps;
};
