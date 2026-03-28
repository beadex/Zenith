#pragma once

#include "Mesh.h"

class Model
{
public:
	// Constructor to initialize the model with a file path to the model data
	Model(ID3D12Device* device, const std::string& path);

	// Draw the model using the provided command list
	void Draw(ID3D12GraphicsCommandList* commandList);

private:
	std::vector<Mesh> m_meshes;
	std::string m_directory;
	ID3D12Device* m_device;

	// Helper function to load the model data from the file
	void LoadModel(const std::string& path);
	void ProcessNode(aiNode* node, const aiScene* scene);
	Mesh ProcessMesh(aiMesh* mesh, const aiScene* scene);

	// Helper function to load textures from the model file
	std::vector<Texture> LoadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName);
};