#include "pch.h"
#include "Model.h"
#include "D3D12ApplicationHelper.h"

using namespace DirectX;

namespace
{
 bool TextureUsesTransparency(const ScratchImage& image)
	{
       const TexMetadata& metadata = image.GetMetadata();
		if (!DirectX::HasAlpha(metadata.format))
		{
			return false;
		}

		bool hasTransparentPixel = false;
		const HRESULT hr = DirectX::EvaluateImage(
			image.GetImages(),
			image.GetImageCount(),
			metadata,
			[&hasTransparentPixel](const XMVECTOR* pixels, size_t width, size_t y)
			{
				UNREFERENCED_PARAMETER(y);
				if (hasTransparentPixel)
				{
					return;
				}

				for (size_t x = 0; x < width; ++x)
				{
					if (XMVectorGetW(pixels[x]) < 0.999f)
					{
						hasTransparentPixel = true;
						return;
					}
				}
			});

		return SUCCEEDED(hr) && hasTransparentPixel;
	}

	void DrawMesh(ID3D12GraphicsCommandList* commandList, Mesh& mesh)
	{
		commandList->SetGraphicsRootConstantBufferView(2, mesh.GetMaterialConstantBufferAddress());
		mesh.Draw(commandList);
	}
}

// ---------------------------------------------------------------------------
// Model
//
// A Model is a higher-level asset built from one or more Mesh objects.
// Its job is to:
//   - load geometry and materials through Assimp
//   - compute bounds for camera framing
//   - load and upload textures
//   - assign texture descriptor indices into each mesh's MaterialData
// ---------------------------------------------------------------------------

Model::Model(CbvSrvUavAllocator* descriptorAllocator, ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const std::string& path) :
	m_boundsMin(FLT_MAX, FLT_MAX, FLT_MAX),
	m_boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX),
	m_descriptorAllocator(descriptorAllocator),
	m_device(device),
	m_commandList(commandList)
{
	LoadModel(path);
}

Model::~Model()
{
	// Texture SRV descriptors live in the allocator's static heap. When the model
	   // goes away, those slots can be returned to the free list and reused.
	if (!m_descriptorAllocator)
	{
		return;
	}

	for (const auto& [_, slot] : m_textureCache)
	{
		m_descriptorAllocator->ReleaseStaticDescriptor(slot);
	}
}

void Model::DrawOpaque(ID3D12GraphicsCommandList* commandList)
{
	for (auto& mesh : m_meshes)
	{
		if (!mesh.IsTransparent())
		{
			DrawMesh(commandList, mesh);
		}
	}
}

void Model::DrawTransparent(ID3D12GraphicsCommandList* commandList, const XMFLOAT3& cameraPosition, const XMFLOAT3& modelOffset)
{
	std::vector<Mesh*> transparentMeshes;
	transparentMeshes.reserve(m_meshes.size());

	for (auto& mesh : m_meshes)
	{
		if (mesh.IsTransparent())
		{
			transparentMeshes.push_back(&mesh);
		}
	}

	std::sort(transparentMeshes.begin(), transparentMeshes.end(),
		[&cameraPosition, &modelOffset](const Mesh* lhs, const Mesh* rhs)
		{
			return lhs->GetCameraDistanceSquared(cameraPosition, modelOffset) > rhs->GetCameraDistanceSquared(cameraPosition, modelOffset);
		});

	for (Mesh* mesh : transparentMeshes)
	{
		DrawMesh(commandList, *mesh);
	}
}

void Model::LoadModel(const std::string& path)
{
	// Assimp converts the file into a scene graph. The code then walks that scene
	  // and converts each aiMesh into the sample's own Mesh class.
	Assimp::Importer importer;

	unsigned int importFlags =
		aiProcess_JoinIdenticalVertices |
		aiProcess_Triangulate |
		aiProcess_PreTransformVertices |
		aiProcess_ConvertToLeftHanded |
		aiProcess_GenSmoothNormals |
		aiProcess_SortByPType |
		aiProcess_GenUVCoords |
		aiProcess_TransformUVCoords;

	// Read file with optimization flags
	const aiScene* scene = importer.ReadFile(path, importFlags);

	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
	{
		OutputDebugStringA(("ERROR::ASSIMP::" + std::string(importer.GetErrorString()) + "\n").c_str());
		return;
	}

	// Save the directory path for loading textures
	const size_t lastSeparator = path.find_last_of("\\/");
	m_directory = (lastSeparator == std::string::npos) ? std::string() : path.substr(0, lastSeparator);

	// Process the root node recursively and collect meshes / textures / bounds.
	ProcessNode(scene->mRootNode, scene);

	if (!m_meshes.empty())
	{
		m_boundsCenter = XMFLOAT3(
			(m_boundsMin.x + m_boundsMax.x) * 0.5f,
			(m_boundsMin.y + m_boundsMax.y) * 0.5f,
			(m_boundsMin.z + m_boundsMax.z) * 0.5f);

		const XMVECTOR minV = XMLoadFloat3(&m_boundsMin);
		const XMVECTOR maxV = XMLoadFloat3(&m_boundsMax);
		const XMVECTOR centerV = XMLoadFloat3(&m_boundsCenter);
		m_boundsRadius = XMVectorGetX(XMVector3Length(maxV - centerV));
	}

	// After all meshes are known, upload textures and assign descriptor indices.
	UploadAllTexturesToGPU();
}

void Model::ProcessNode(aiNode* node, const aiScene* scene)
{
	// Assimp stores a hierarchy of nodes. Each node can reference meshes and can
	// also have children, so the scene is traversed recursively.
	OutputDebugStringA(("ProcessNode: " + std::string(node->mName.C_Str()) +
		" meshes=" + std::to_string(node->mNumMeshes) +
		" children=" + std::to_string(node->mNumChildren) + "\n").c_str());
	// 1. Process all the node's meshes (if any)
	for (UINT i = 0; i < node->mNumMeshes; i++)
	{
		aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		m_meshes.push_back(std::move(ProcessMesh(mesh, scene)));
	}

	// 2. Then do the same for each of its children
	for (UINT i = 0; i < node->mNumChildren; i++)
	{
		ProcessNode(node->mChildren[i], scene);
	}
}

Mesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene)
{
	// This function converts Assimp's aiMesh into CPU-side arrays that the Mesh
	 // constructor will then upload to GPU buffers.
	std::vector<Vertex> vertices;
	std::vector<UINT> indices;
	std::vector<Texture> textures;
	vertices.reserve(mesh->mNumVertices);
	indices.reserve(mesh->mNumFaces * 3);

	for (UINT i = 0; i < mesh->mNumVertices; i++)
	{
		Vertex vertex = {};

		// Process vertex positions
		XMFLOAT3 vector;

		vector.x = mesh->mVertices[i].x;
		vector.y = mesh->mVertices[i].y;
		vector.z = mesh->mVertices[i].z;

		vertex.Position = vector;

		m_boundsMin.x = (std::min)(m_boundsMin.x, vector.x);
		m_boundsMin.y = (std::min)(m_boundsMin.y, vector.y);
		m_boundsMin.z = (std::min)(m_boundsMin.z, vector.z);
		m_boundsMax.x = (std::max)(m_boundsMax.x, vector.x);
		m_boundsMax.y = (std::max)(m_boundsMax.y, vector.y);
		m_boundsMax.z = (std::max)(m_boundsMax.z, vector.z);

		// Process vertex normals
		if (mesh->HasNormals())
		{
			vector.x = mesh->mNormals[i].x;
			vector.y = mesh->mNormals[i].y;
			vector.z = mesh->mNormals[i].z;

			vertex.Normal = vector;
		}

		// Process texture coordinates (only the first set)
		if (mesh->mTextureCoords[0])
		{
			XMFLOAT2 vec;
			vec.x = mesh->mTextureCoords[0][i].x;
			vec.y = mesh->mTextureCoords[0][i].y;

			vertex.TexCoord = vec;
		}
		else
		{
			vertex.TexCoord = XMFLOAT2(0.0f, 0.0f);
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

	// Gather diffuse/specular textures referenced by the material so they can be
	// uploaded later and represented by descriptor indices in MaterialData.
	if (mesh->mMaterialIndex >= 0)
	{
		aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
		MaterialData materialData = {};
		aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
		if (material->Get(AI_MATKEY_BASE_COLOR, baseColor) != aiReturn_SUCCESS)
		{
			material->Get(AI_MATKEY_COLOR_DIFFUSE, baseColor);
		}
		materialData.baseColorFactor = XMFLOAT4(baseColor.r, baseColor.g, baseColor.b, baseColor.a);

		const aiTextureType diffuseTextureType =
			material->GetTextureCount(aiTextureType_BASE_COLOR) > 0 ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE;

		aiString alphaMode;
		const bool hasExplicitAlphaMode = material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == aiReturn_SUCCESS;
		const bool isBlendAlphaMode = hasExplicitAlphaMode && _stricmp(alphaMode.C_Str(), "BLEND") == 0;
		const bool isMaskAlphaMode = hasExplicitAlphaMode && _stricmp(alphaMode.C_Str(), "MASK") == 0;
		const bool isOpaqueAlphaMode = hasExplicitAlphaMode && _stricmp(alphaMode.C_Str(), "OPAQUE") == 0;

		bool isTransparent = false;
		if (isBlendAlphaMode)
		{
			isTransparent = true;
		}
		else if (!hasExplicitAlphaMode)
		{
			isTransparent = material->GetTextureCount(aiTextureType_OPACITY) > 0;
			isTransparent = isTransparent || materialData.baseColorFactor.w < 0.999f;
		}

		UNREFERENCED_PARAMETER(isMaskAlphaMode);
		UNREFERENCED_PARAMETER(isOpaqueAlphaMode);
		textures.reserve(
			material->GetTextureCount(diffuseTextureType) +
			material->GetTextureCount(aiTextureType_SPECULAR) +
			material->GetTextureCount(aiTextureType_OPACITY));

		// Load diffuse/base-color textures.
		std::vector<Texture> diffuseMaps = LoadMaterialTextures(material, diffuseTextureType, "texture_diffuse", scene);
		for (auto& tex : diffuseMaps)
		{
          if (!hasExplicitAlphaMode)
			{
				isTransparent = isTransparent || tex.hasAlpha;
			}
			textures.push_back(std::move(tex));
		}

		// Load specular textures
		std::vector<Texture> specularMaps = LoadMaterialTextures(material, aiTextureType_SPECULAR, "texture_specular", scene);
		for (auto& tex : specularMaps)
			textures.push_back(std::move(tex));

		std::vector<Texture> opacityMaps = LoadMaterialTextures(material, aiTextureType_OPACITY, "texture_opacity", scene);
		for (auto& tex : opacityMaps)
		{
           if (!hasExplicitAlphaMode)
			{
				isTransparent = true;
			}
			textures.push_back(std::move(tex));
		}

		Mesh result(m_device, m_commandList, std::move(vertices), std::move(indices), std::move(textures), isTransparent);
		result.SetMaterialData(materialData);
		return result;
	}

	Mesh result(m_device, m_commandList, std::move(vertices), std::move(indices), std::move(textures), false);
	result.SetMaterialData(MaterialData{});
	return result;
}

std::vector<Texture> Model::LoadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& typeName, const aiScene* scene)
{
	std::vector<Texture> textures;

	// The cache prevents loading and uploading the same texture file multiple
	// times when several meshes or materials reference it.

	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++)
	{
		aiString str;
		mat->GetTexture(type, i, &str);

		Texture texture;
		texture.type = typeName;

		// Resolve full path TRƯỚC khi check cache
		const aiTexture* embeddedTex = scene->GetEmbeddedTexture(str.C_Str());
		if (embeddedTex)
		{
			texture.path = str.C_Str(); // embedded dùng key gốc "*0", "*1"...
		}
		else
		{
			texture.path = m_directory + "/" + std::string(str.C_Str()); // full path ngay
		}

		OutputDebugStringA(("  [LoadMat] path='" + texture.path +
			"' cached=" + std::to_string(m_textureCache.count(texture.path)) + "\n").c_str());

		if (m_loadedPaths.count(texture.path))
		{
			texture.hasAlpha = m_textureTransparencyCache[texture.path];
			textures.push_back(std::move(texture));
			continue;
		}

		// Bây giờ check cache với đúng key
		if (m_textureCache.count(texture.path))
		{
			texture.heapIndex = m_textureCache[texture.path];
			texture.hasAlpha = m_textureTransparencyCache[texture.path];
			textures.push_back(std::move(texture));
			continue; // skip load hoàn toàn — ScratchImage không bị tạo
		}

		m_loadedPaths.insert(texture.path);

		HRESULT hr = E_FAIL;

		// Check if the texture is embedded in the model file
		if (embeddedTex)
		{
			const auto* data = reinterpret_cast<const uint8_t*>(embeddedTex->pcData);

			if (embeddedTex->mHeight == 0)
			{
				// Compressed image stored as a raw byte blob (PNG, JPG, DDS, etc.)
				// mWidth holds the buffer size in bytes
				const size_t size = embeddedTex->mWidth;

				if (strncmp(embeddedTex->achFormatHint, "dds", 3) == 0)
				{
					hr = LoadFromDDSMemory(data, size, DDS_FLAGS_NONE, nullptr, texture.image);
				}
				else
				{
					// PNG, JPG, BMP, etc.
					hr = LoadFromWICMemory(data, size, WIC_FLAGS_NONE, nullptr, texture.image);
				}
			}
			else
			{
				// Uncompressed BGRA8 raw pixel data (one aiTexel = 4 bytes, B8G8R8A8)
				Image rawImage = {};
				rawImage.width = embeddedTex->mWidth;
				rawImage.height = embeddedTex->mHeight;
				rawImage.format = DXGI_FORMAT_B8G8R8A8_UNORM;
				rawImage.rowPitch = embeddedTex->mWidth * sizeof(aiTexel);
				rawImage.slicePitch = rawImage.rowPitch * embeddedTex->mHeight;
				rawImage.pixels = reinterpret_cast<uint8_t*>(embeddedTex->pcData);

				hr = texture.image.InitializeFromImage(rawImage);
			}

			if (FAILED(hr))
			{
				OutputDebugStringA(("ERROR: Failed to load embedded texture: " + std::string(str.C_Str()) + "\n").c_str());
				m_loadedPaths.erase(texture.path);
				continue;
			}
		}
		else
		{
			std::wstring widePath(texture.path.begin(), texture.path.end());

			size_t dotPos = texture.path.rfind('.');
			if (dotPos != std::string::npos)
			{
				std::string ext = texture.path.substr(dotPos);
				if (_stricmp(ext.c_str(), ".dds") == 0)
				{
					hr = LoadFromDDSFile(widePath.c_str(), DDS_FLAGS_NONE, nullptr, texture.image);
				}
				else if (_stricmp(ext.c_str(), ".tga") == 0)
				{
					hr = LoadFromTGAFile(widePath.c_str(), TGA_FLAGS_NONE, nullptr, texture.image);
				}
				else
				{
					hr = LoadFromWICFile(widePath.c_str(), WIC_FLAGS_NONE, nullptr, texture.image);
				}
			}

			if (FAILED(hr))
			{
				OutputDebugStringA(("ERROR: Failed to load texture: " + texture.path + "\n").c_str());
				m_loadedPaths.erase(texture.path);
				continue;
			}
		}

       texture.hasAlpha = (type == aiTextureType_OPACITY) || TextureUsesTransparency(texture.image);
		m_textureTransparencyCache[texture.path] = texture.hasAlpha;

		textures.push_back(std::move(texture));
	}

	return textures;
}

void Model::UploadAllTexturesToGPU()
{
	// Each mesh receives a compact MaterialData struct containing descriptor-slot
	   // indices instead of direct texture objects. The shader later uses those
	   // indices to fetch from the descriptor heap.
	for (int meshIdx = 0; meshIdx < m_meshes.size(); meshIdx++)
	{
		auto& mesh = m_meshes[meshIdx];
		MaterialData matData = mesh.GetMaterialData();
		bool diffuseSet = false;
		bool specularSet = false;
		bool opacitySet = false;

		OutputDebugStringA(("=== Mesh " + std::to_string(meshIdx) +
			" has " + std::to_string(mesh.GetTextures().size()) + " textures ===\n").c_str());


		for (auto& tex : mesh.GetTextures())
		{
			UINT slot = UploadTextureToHeap(tex); // Upload and get slot

			if (tex.type == "texture_diffuse")
			{
				if (!diffuseSet) {
					matData.diffuseStartIndex = slot;
					matData.numDiffuse = 1;
					diffuseSet = true;
				}
			}
			else if (tex.type == "texture_specular")
			{
				if (!specularSet) {
					matData.specularStartIndex = slot;
					matData.numSpecular = 1;
					specularSet = true;
				}
			}
			else if (tex.type == "texture_opacity")
			{
				if (!opacitySet) {
					matData.opacityStartIndex = slot;
					matData.numOpacity = 1;
					opacitySet = true;
				}
			}
		}

		mesh.SetMaterialData(matData);
	}
}

UINT Model::UploadTextureToHeap(Texture& texture)
{
	// If the texture was already uploaded, simply reuse the existing descriptor
	// slot and avoid duplicate GPU resources.
	OutputDebugStringA(("  [Upload] path='" + texture.path +
		"' cached=" + std::to_string(m_textureCache.count(texture.path)) + "\n").c_str());
	auto it = m_textureCache.find(texture.path);
	if (it != m_textureCache.end())
	{
		texture.heapIndex = it->second;
		texture.image.Release();
		return it->second; // trả về slot cũ, không upload lại
	}

	UINT slot = m_descriptorAllocator->AllocateStaticDescriptor();

	// Create the final GPU texture resource in default memory.
	ComPtr<ID3D12Resource> textureResource;
	ThrowIfFailed(DirectX::CreateTexture(
		m_device,
		texture.image.GetMetadata(),
		&textureResource));

	// Prepare subresource upload data, then stage it through an UPLOAD heap.
	std::vector<D3D12_SUBRESOURCE_DATA> subresources;
	ThrowIfFailed(DirectX::PrepareUpload(
		m_device,
		texture.image.GetImages(),
		texture.image.GetImageCount(),
		texture.image.GetMetadata(),
		subresources));

	const UINT64 uploadSize = GetRequiredIntermediateSize(
		textureResource.Get(), 0, (UINT)subresources.size());

	ComPtr<ID3D12Resource> uploadBuffer;
	ThrowIfFailed(m_device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuffer)));

	m_textureUploadBuffers.push_back(uploadBuffer);
	m_textureResources.push_back(textureResource);

	UpdateSubresources(m_commandList, textureResource.Get(),
		uploadBuffer.Get(), 0, 0,
		(UINT)subresources.size(), subresources.data());

	// After the copy is recorded, transition so shaders can sample from it.
	auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		textureResource.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_commandList->ResourceBarrier(1, &barrier);

	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_descriptorAllocator->GetStaticCpuHandle(slot);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = texture.image.GetMetadata().format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = (UINT)texture.image.GetMetadata().mipLevels;

	m_device->CreateShaderResourceView(textureResource.Get(), &srvDesc, handle);

	texture.heapIndex = slot;
	m_textureCache[texture.path] = slot; // Cache slot index
	texture.image.Release();
	return slot;
}

void Model::ReleaseUploadBuffers()
{
	m_textureUploadBuffers.clear();
	for (auto& mesh : m_meshes)
		mesh.ReleaseUploadBuffers();
}
