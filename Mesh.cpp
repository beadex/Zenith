#include "pch.h"
#include "Mesh.h"
#include "D3D12ApplicationHelper.h"

Mesh::Mesh(ID3D12Device* device, std::vector<Vertex> vertices, std::vector<UINT> indices, std::vector<Texture> textures) :
	m_vertices(vertices),
	m_indices(indices),
	m_textures(textures),
	m_vertexCount(static_cast<UINT>(vertices.size())),
	m_indexCount(static_cast<UINT>(indices.size()))
{
	CreateBuffers(device);
}

void Mesh::Draw(ID3D12GraphicsCommandList* commandList)
{
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
	commandList->IASetIndexBuffer(&m_indexBufferView);
	commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

void Mesh::CreateBuffers(ID3D12Device* device)
{
	const UINT vertexBufferSize = m_vertexCount * sizeof(Vertex);
	const UINT indexBufferSize = m_indexCount * sizeof(UINT);

	// 1. Create vertex buffer
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_vertexBuffer)
	));

	// 2. Copy data from RAM to Vertex Buffer
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
	ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, m_vertices.data(), vertexBufferSize);
	m_vertexBuffer->Unmap(0, nullptr);

	// 3. Setup vertex buffer view
	m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
	m_vertexBufferView.StrideInBytes = sizeof(Vertex);
	m_vertexBufferView.SizeInBytes = vertexBufferSize;

	// 4. Create index buffer
	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_indexBuffer)
	));

	// 5. Copy data from RAM to Index Buffer
	UINT8* pIndexDataBegin;
	ThrowIfFailed(m_indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
	memcpy(pIndexDataBegin, m_indices.data(), indexBufferSize);
	m_indexBuffer->Unmap(0, nullptr);

	// 6. Setup index buffer view
	m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
	m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
	m_indexBufferView.SizeInBytes = indexBufferSize;
}
