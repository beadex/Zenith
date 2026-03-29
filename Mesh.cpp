#include "pch.h"
#include "Mesh.h"
#include "D3D12ApplicationHelper.h"

Mesh::Mesh(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, std::vector<Vertex> vertices, std::vector<UINT> indices, std::vector<Texture> textures) :
	m_vertices(vertices),
	m_indices(indices),
	m_textures(textures),
	m_vertexCount(static_cast<UINT>(vertices.size())),
	m_indexCount(static_cast<UINT>(indices.size()))
{
	CreateBuffers(device, commandList);
}

void Mesh::Draw(ID3D12GraphicsCommandList* commandList)
{
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
	commandList->IASetIndexBuffer(&m_indexBufferView);
	commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

void Mesh::CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
{
	const UINT vertexBufferSize = m_vertexCount * sizeof(Vertex);
	const UINT indexBufferSize = m_indexCount * sizeof(UINT);

	// 1. Create GPU-only DEFAULT heap buffers
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&m_vertexBuffer)));

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&m_indexBuffer)));

	// 2. Create CPU-visible UPLOAD staging buffers (members, NOT locals)
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_vertexUploadBuffer)));

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_indexUploadBuffer)));

	// 3. Map upload buffers and copy CPU data in
	CD3DX12_RANGE readRange(0, 0);
	UINT8* pMapped = nullptr;

	ThrowIfFailed(m_vertexUploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pMapped)));
	memcpy(pMapped, m_vertices.data(), vertexBufferSize);
	m_vertexUploadBuffer->Unmap(0, nullptr);

	ThrowIfFailed(m_indexUploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pMapped)));
	memcpy(pMapped, m_indices.data(), indexBufferSize);
	m_indexUploadBuffer->Unmap(0, nullptr);

	// 4. Record GPU copy: upload heap -> default heap
	commandList->CopyBufferRegion(m_vertexBuffer.Get(), 0, m_vertexUploadBuffer.Get(), 0, vertexBufferSize);
	commandList->CopyBufferRegion(m_indexBuffer.Get(), 0, m_indexUploadBuffer.Get(), 0, indexBufferSize);

	// 5. Transition to shader-readable states
	const D3D12_RESOURCE_BARRIER barriers[] = {
		CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
		CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
	};
	commandList->ResourceBarrier(_countof(barriers), barriers);

	// 6. Setup buffer views
	m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
	m_vertexBufferView.StrideInBytes = sizeof(Vertex);
	m_vertexBufferView.SizeInBytes = vertexBufferSize;

	m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
	m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;  // Bug 3 fix: was R16_UINT
	m_indexBufferView.SizeInBytes = indexBufferSize;
}
