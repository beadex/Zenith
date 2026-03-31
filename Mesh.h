#pragma once

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct Vertex
{
	XMFLOAT3 Position;
	XMFLOAT3 Normal;
	XMFLOAT2 TexCoord;
	XMFLOAT3 Tangent;
	XMFLOAT3 Bitangent;
};

struct MaterialData
{
	// Texture descriptor indices copied into the shader-visible heap each frame.
	UINT diffuseStartIndex = 0;
	UINT specularStartIndex = 0;
	UINT opacityStartIndex = 0;
	UINT normalStartIndex = 0;
	// Texture counts let the shader know whether a given texture type exists.
	UINT numDiffuse = 0;
	UINT numSpecular = 0;
	UINT numOpacity = 0;
	UINT numNormal = 0;
	// glTF alpha handling:
	   //   0 = OPAQUE
	   //   1 = MASK
	   //   2 = BLEND
	UINT alphaMode = 0;
	// Used only by glTF MASK materials.
	float alphaCutoff = 0.5f;
	XMFLOAT2 padding = XMFLOAT2(0.0f, 0.0f);
	// Base color factor is the lightweight way this sample now respects glTF
	// material tint and alpha without implementing a full PBR shading model.
	XMFLOAT4 baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
};

static_assert(offsetof(MaterialData, baseColorFactor) == 48, "MaterialData must match HLSL packing.");

struct Texture {
	std::string type; // For example: "texture_diffuse", "texture_specular", "texture_normal"
	std::string path; // File path or embedded key
	ScratchImage image;
	UINT heapIndex = UINT_MAX;
	bool hasAlpha = false;
};

class Mesh
{
public:
	// Constructor to initialize the mesh with vertices, indices, and textures
	Mesh(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, std::vector<Vertex> vertices, std::vector<UINT> indices, std::vector<Texture> textures, bool isTransparent, bool isDoubleSided);

	// Draw the mesh using the provided command list
	void Draw(ID3D12GraphicsCommandList* commandList);

	// Call this after EndUpload() to free staging memory and CPU-side data
	void ReleaseUploadBuffers();

	std::vector<Texture>& GetTextures() { return m_textures; }
	bool IsTransparent() const { return m_isTransparent; }
	// doubleSided is stored per mesh so the renderer can choose the correct PSO
	  // instead of globally disabling culling for an entire pass.
	bool IsDoubleSided() const { return m_isDoubleSided; }
	const MaterialData& GetMaterialData() const { return m_materialData; }
	float GetCameraDistanceSquared(const XMFLOAT3& cameraPosition, const XMFLOAT3& modelOffset) const;

	D3D12_GPU_VIRTUAL_ADDRESS GetMaterialConstantBufferAddress() const
	{
		return m_materialConstantBuffer->GetGPUVirtualAddress();
	}

	void SetMaterialData(const MaterialData& data);
private:
	// Resource on CPU
	std::vector<Vertex> m_vertices;
	std::vector<UINT> m_indices;
	std::vector<Texture> m_textures;
	MaterialData m_materialData;
	bool m_isTransparent = false;
	bool m_isDoubleSided = false;
	XMFLOAT3 m_boundsCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);

	// Resource on GPU
	ComPtr<ID3D12Resource> m_vertexBuffer;
	ComPtr<ID3D12Resource> m_indexBuffer;
	ComPtr<ID3D12Resource> m_materialConstantBuffer;
	ComPtr<ID3D12Resource> m_vertexUploadBuffer;
	ComPtr<ID3D12Resource> m_indexUploadBuffer;
	ComPtr<ID3D12Resource> m_materialUploadConstantBuffer;

	// Buffer views
	D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW m_indexBufferView;

	UINT m_vertexCount;
	UINT m_indexCount;

	UINT8* m_mappedMaterialData = nullptr;

	// Helper function to create vertex and index buffers on the GPU
	void CreateVertexAndIndexBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
	void CreateMaterialConstantBuffer(ID3D12Device* device);
};