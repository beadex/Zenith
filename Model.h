#pragma once

#include "Mesh.h"
#include "DescriptorAllocator.h"

class Model
{
public:
	// Constructor to initialize the model with a file path to the model data
	Model(CbvSrvUavAllocator* descriptorAllocator, ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const std::string& path);
	~Model();

  // Rendering is split into four buckets:
	//   - opaque single-sided
	//   - opaque double-sided
	//   - transparent single-sided
	//   - transparent double-sided
	//
	// The caller selects which bucket to draw by passing the desired flags.
	void DrawOpaque(ID3D12GraphicsCommandList* commandList, bool doubleSided);
	void DrawTransparent(ID3D12GraphicsCommandList* commandList, const XMFLOAT3& cameraPosition, const XMFLOAT3& modelOffset, bool doubleSided);
	bool IsLoaded() const { return !m_meshes.empty(); }

	void ReleaseUploadBuffers();
	XMFLOAT3 GetBoundsMin() const { return m_boundsMin; }
	XMFLOAT3 GetBoundsCenter() const { return m_boundsCenter; }
	float GetBoundsRadius() const { return m_boundsRadius; }
private:
	std::unordered_map<std::string, UINT> m_textureCache;
   // Stores whether a texture actually uses transparency, not just whether its
	// format happens to contain an alpha channel. This keeps opaque RGBA textures
	// out of the transparent pass.
	std::unordered_map<std::string, bool> m_textureTransparencyCache;
	std::unordered_set<std::string> m_loadedPaths;
	std::vector<Mesh> m_meshes;
	XMFLOAT3 m_boundsMin;
	XMFLOAT3 m_boundsMax;
	XMFLOAT3 m_boundsCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
	float m_boundsRadius = 0.0f;
	std::string m_directory;
	CbvSrvUavAllocator* m_descriptorAllocator;
	ID3D12Device* m_device;
	ID3D12GraphicsCommandList* m_commandList;

	std::vector<ComPtr<ID3D12Resource>> m_textureUploadBuffers;
	std::vector<ComPtr<ID3D12Resource>> m_textureResources;

	// Helper function to load the model data from the file
	void LoadModel(const std::string& path);
	void ProcessNode(aiNode* node, const aiScene* scene);
	Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene);

	// Helper function to load textures from the model file
	std::vector<Texture> LoadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName, const aiScene* scene);

	void UploadAllTexturesToGPU();  // Call when the model is loaded to upload all textures to the GPU and create SRVs for them
	UINT UploadTextureToHeap(Texture& texture);
};