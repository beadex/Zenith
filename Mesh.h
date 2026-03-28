#pragma once

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct Vertex
{
	XMFLOAT3 Position;
	XMFLOAT3 Normal;
	XMFLOAT2 TexCoord;
};

struct Texture {
	std::string type; // For example: "texture_diffuse", "texture_specular"
	std::string path; // File path to the texture image
};

class Mesh
{
public:
	// Constructor to initialize the mesh with vertices, indices, and textures
	Mesh(ID3D12Device* device, std::vector<Vertex> vertices, std::vector<UINT> indices, std::vector<Texture> textures);

	// Draw the mesh using the provided command list
	void Draw(ID3D12GraphicsCommandList* commandList);
private:
	// Resource on CPU
	std::vector<Vertex> m_vertices;
	std::vector<UINT> m_indices;
	std::vector<Texture> m_textures;

	// Resource on GPU
	ComPtr<ID3D12Resource> m_vertexBuffer;
	ComPtr<ID3D12Resource> m_indexBuffer;

	// Buffer views
	D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW m_indexBufferView;

	UINT m_vertexCount;
	UINT m_indexCount;

	// Helper function to create vertex and index buffers on the GPU
	void CreateBuffers(ID3D12Device* device);
};