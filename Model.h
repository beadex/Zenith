#pragma once

#include "Mesh.h"

class Model
{
public:
	// Constructor to initialize the model with a file path to the model data
	Model(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const std::string& path);

	// Draw the model using the provided command list
	void Draw(ID3D12GraphicsCommandList* commandList);
	bool IsLoaded() const { return !m_meshes.empty(); }

	void ReleaseUploadBuffers();

	ID3D12DescriptorHeap* GetSRVHeap() const { return m_srvHeap.Get(); }
private:
	std::unordered_map<std::string, UINT> m_textureCache;
	std::unordered_set<std::string> m_loadedPaths;
	std::vector<Mesh> m_meshes;
	std::string m_directory;
	ID3D12Device* m_device;
	ID3D12GraphicsCommandList* m_commandList;

	std::vector<ComPtr<ID3D12Resource>> m_textureUploadBuffers;
	std::vector<ComPtr<ID3D12Resource>> m_textureResources;

	ComPtr<ID3D12DescriptorHeap> m_srvHeap;
	UINT m_descriptorSize = 0;
	UINT m_nextFreeSlot = 0;

	static constexpr UINT MAX_TEXTURES = 256;

	// Helper function to load the model data from the file
	void LoadModel(const std::string& path);
	void ProcessNode(aiNode* node, const aiScene* scene);
	Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene);

	// Helper function to load textures from the model file
	std::vector<Texture> LoadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName, const aiScene* scene);

	void CreateSRVHeap();
	void UploadAllTexturesToGPU();  // Call when the model is loaded to upload all textures to the GPU and create SRVs for them
	UINT UploadTextureToHeap(Texture& texture);
};