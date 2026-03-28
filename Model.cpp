#include "pch.h"
#include "Model.h"

Model::Model(ID3D12Device* device, const std::string& path) :
	m_device(device)
{
	LoadModel(path);
}

void Model::Draw(ID3D12GraphicsCommandList* commandList)
{
	// Duyệt qua toàn bộ danh sách các Mesh của Model
	for (unsigned int i = 0; i < m_meshes.size(); i++)
	{
		// Ra lệnh cho từng Mesh thực hiện việc Bind Buffer và DrawIndexed
		m_meshes[i].Draw(commandList);
	}
}

void Model::LoadModel(const std::string& path)
{
	Assimp::Importer importer;

	// Read file with optimization flags: Triangulate, FlipUVs
	const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		OutputDebugStringA(("ERROR::ASSIMP::" + std::string(importer.GetErrorString()) + "\n").c_str());
		return;
	}

	// Save the directory path for loading textures
	m_directory = path.substr(0, path.find_last_of('/'));

	// Process the root node recursively
	ProcessNode(scene->mRootNode, scene);
}

void Model::ProcessNode(aiNode* node, const aiScene* scene)
{
	// 1. Process all the node's meshes (if any)
	for (UINT i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		m_meshes.push_back(ProcessMesh(mesh, scene));
	}

	// 2. Then do the same for each of its children
	for (UINT i = 0; i < node->mNumChildren; i++)
	{
		ProcessNode(node->mChildren[i], scene);
	}
}

Mesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
{
	std::vector<Vertex> vertices;
	std::vector<UINT> indices;
	std::vector<Texture> textures;

	for (UINT i = 0; i < mesh->mNumVertices; i++)
	{
		Vertex vertex;

		// Process vertex positions
		vertex.Position.x = mesh->mVertices[i].x;
		vertex.Position.y = mesh->mVertices[i].y;
		vertex.Position.z = mesh->mVertices[i].z;

		// Process vertex normals
		if (mesh->HasNormals())
		{
			vertex.Normal.x = mesh->mNormals[i].x;
			vertex.Normal.y = mesh->mNormals[i].y;
			vertex.Normal.z = mesh->mNormals[i].z;
		}

		// Process texture coordinates (only the first set)
		if (mesh->mTextureCoords[0])
		{
			vertex.TexCoord.x = mesh->mTextureCoords[0][i].x;
			vertex.TexCoord.y = mesh->mTextureCoords[0][i].y;
		}
		else
		{
			vertex.TexCoord = DirectX::XMFLOAT2(0.0f, 0.0f);
		}

		vertices.push_back(vertex);
	}

	// Process indices for each face (triangle)
	for (UINT i = 0; i < mesh->mNumFaces; i++)
	{
		aiFace face = mesh->mFaces[i];
		for (UINT j = 0; j < face.mNumIndices; j++)
		{
			indices.push_back(face.mIndices[j]);
		}
	}

	// Process material textures
	if (mesh->mMaterialIndex >= 0)
	{
		aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];

		// Load diffuse textures
		std::vector<Texture> diffuseMaps = LoadMaterialTextures(material, aiTextureType_DIFFUSE, "texture_diffuse");
		textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());
	}

	return Mesh(m_device, vertices, indices, textures);
}

std::vector<Texture> Model::LoadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName)
{
	std::vector<Texture> textures;

	// Kiểm tra xem Material này có bao nhiêu Texture thuộc loại 'type' (ví dụ Diffuse)
	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str); // Lấy tên file/đường dẫn ảnh

		// Kiểm tra xem Texture này đã được load trước đó chưa (để tránh lãng phí VRAM)
		// Lưu ý: Hiện tại ta chỉ lưu struct thông tin, sau này sẽ nâng cấp lên TextureManager

		Texture texture;
		texture.type = typeName;

		// str.C_Str() có thể là "textures/wood.jpg"
		// Ta cần gộp với m_directory để có đường dẫn đầy đủ: "Assets/Models/textures/wood.jpg"
		texture.path = m_directory + "/" + std::string(str.C_Str());

		textures.push_back(texture);
	}

	return textures;
}


