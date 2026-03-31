#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"
#include "DescriptorAllocator.h"

#pragma comment(lib, "Comdlg32.lib")

// ---------------------------------------------------------------------------
// ZenithRenderEngine
//
// This file is the "high-level renderer" for the sample. It sits above the
// lower-level D3D12RenderContext and is responsible for:
//   1. creating the root signature and pipeline states
//   2. creating per-frame constant buffers
//   3. loading a model and framing the camera around it
//   4. updating scene / lighting data every frame
//   5. binding descriptors and issuing draw calls
//
// A useful mental model is:
//   - D3D12RenderContext = platform / GPU plumbing
//   - ZenithRenderEngine = actual scene rendering policy
// ---------------------------------------------------------------------------

namespace
{
	// Helper used when the Win32 file dialog returns a UTF-16 path but the
	 // model loader expects UTF-8. The conversion itself is not D3D12-specific;
	 // it is just glue between Win32 and the asset-loading layer.
	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
		{
			return {};
		}

		const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (requiredSize <= 1)
		{
			return {};
		}

		std::vector<char> buffer(static_cast<size_t>(requiredSize));
		WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), requiredSize, nullptr, nullptr);

		return std::string(buffer.data());
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ZenithRenderEngine::ZenithRenderEngine(UINT width, UINT height, std::wstring name)
	: D3D12Application(width, height, name)
	, m_model(nullptr)
	, m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height))
	, m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
	, m_sceneDataCbData{}
	, m_pSceneDataCbvDataBegin(nullptr)
	, m_lightingDataCbData{}
	, m_pLightingDataCbvDataBegin(nullptr)
{
	// Configure the camera lens once we know the viewport size
	m_camera.SetLens(XM_PIDIV4,
		static_cast<float>(width) / static_cast<float>(height),
		0.1f, 500.0f);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ZenithRenderEngine::OnInit()
{
	// 1. Initialize D3D12 Context (Device, Swap Chain, Command Queue, RTV Heap...)
	m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

	// 2. Create all long-lived rendering objects.
	   //
	   // These are the "engine state" objects that usually live for most of the
	   // program: root signature, PSOs, persistent upload buffers, and the grid.
	CreateRootSignature();
	CreatePipelineState();
	CreateGridPipelineState();
	CreateSceneDataConstantBuffer();
	CreateLightingDataConstantBuffer();
	CreateGridVertexBuffer();
	UpdateLightingMenuState();
}

bool ZenithRenderEngine::OnCommand(UINT commandId)
{
	switch (commandId)
	{
	case IDM_FILE_LOAD_MODEL:
	{
		wchar_t filePath[MAX_PATH] = {};
		OPENFILENAMEW openFileName = {};
		openFileName.lStructSize = sizeof(openFileName);
		openFileName.hwndOwner = Win32Application::GetHwnd();
		openFileName.lpstrFile = filePath;
		openFileName.nMaxFile = _countof(filePath);
		openFileName.lpstrFilter = L"3D Model Files\0*.obj;*.fbx;*.dae;*.gltf;*.glb;*.3ds;*.stl;*.ply\0All Files\0*.*\0\0";
		openFileName.nFilterIndex = 1;
		openFileName.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

		if (GetOpenFileNameW(&openFileName))
		{
			LoadModelFromPath(filePath);
		}

		return true;
	}
	case IDM_ABOUT:
		MessageBoxW(
			Win32Application::GetHwnd(),
			L"Zenith\nDirect3D 12 model viewer\n\nUse Files > Load Model to open a supported 3D asset.",
			GetTitle(),
			MB_OK | MB_ICONINFORMATION);
		return true;
	case IDM_RENDER_IMAGE:
	{
		wchar_t filePath[MAX_PATH] = L"render.png";
		OPENFILENAMEW saveFileName = {};
		saveFileName.lStructSize = sizeof(saveFileName);
		saveFileName.hwndOwner = Win32Application::GetHwnd();
		saveFileName.lpstrFile = filePath;
		saveFileName.nMaxFile = _countof(filePath);
		saveFileName.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0\0";
		saveFileName.nFilterIndex = 1;
		saveFileName.lpstrDefExt = L"png";
		saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

		if (GetSaveFileNameW(&saveFileName))
		{
			m_pendingRenderImagePath = filePath;
		}

		return true;
	}
	case IDM_VIEW_DIRECTIONAL_LIGHT:
		m_directionalLightEnabled = !m_directionalLightEnabled;
		UpdateLightingMenuState();
		return true;
	default:
		return false;
	}
}

void ZenithRenderEngine::UpdateLightingMenuState() const
{
	HMENU menu = GetMenu(Win32Application::GetHwnd());
	if (!menu)
	{
		return;
	}

	CheckMenuItem(
		menu,
		IDM_VIEW_DIRECTIONAL_LIGHT,
		MF_BYCOMMAND | (m_directionalLightEnabled ? MF_CHECKED : MF_UNCHECKED));
	DrawMenuBar(Win32Application::GetHwnd());
}

void ZenithRenderEngine::CreateRootSignature()
{
	auto device = m_renderContext->GetDevice();

	D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

	// This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
	featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
	{
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
	}

	// Root-signature layout overview:
	//   Slot 0: SRV descriptor table for model textures (t0...)
	//   Slot 1: CBV descriptor table for SceneData (b0)
	//   Slot 2: Root CBV for per-mesh MaterialData (b1)
	//   Slot 3: CBV descriptor table for LightingData (b2)
	//
	// The main rule being used here is:
	//   - heap-managed resources -> descriptor tables
	//   - tiny per-draw/per-mesh buffer addresses -> root CBV is still fine
	CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
	ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
	ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

	// 1. Tạo Root Signature
	CD3DX12_ROOT_PARAMETER1 rootParameters[4];
	// Root param 0: SRV heap (texture array) → pixel shader
	rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

	// Root param 1: SceneData b0 → dynamic CBV descriptor
	rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_VERTEX);

	// Root param 2: MaterialData b1 → pixel shader
	rootParameters[2].InitAsConstantBufferView(1, 0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_PIXEL);

	// Root param 3: LightingData b2 → dynamic CBV descriptor
	rootParameters[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);

	// Static samplers are baked into the root signature instead of stored in a
	   // descriptor heap. This keeps sampler management simple for a beginner sample.
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_ANISOTROPIC;
	sampler.MaxAnisotropy = 8;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, rootSignatureFlags);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
	ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

void ZenithRenderEngine::CreatePipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	// Load file shaders.hlsl (bạn cần tạo file này trong project)
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	// 2. Định nghĩa Input Layout
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// A PSO in D3D12 is a large immutable bundle of GPU state.
	  // Instead of setting many tiny states dynamically like older APIs, D3D12
	  // expects most of the fixed-function configuration to be pre-baked here.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateGridPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	// Load file shaders.hlsl (bạn cần tạo file này trong project)
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_GridPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// The grid uses a separate PSO because it has different rendering needs:
	// line topology and depth writes disabled, while still depth-testing against
	// the rest of the scene.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_gridPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// D3D12 requires CBVs to be 256-byte aligned.
	  // This buffer holds transforms that are updated every frame.
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_sceneDataConstantBuffer)));

	// UPLOAD heaps are CPU-visible, so we map once and keep the pointer alive.
	 // That makes per-frame updates very simple for learning purposes.
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_sceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pSceneDataCbvDataBegin)));
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
}

void ZenithRenderEngine::CreateLightingDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// Same idea as SceneData: small per-frame buffer, stored in an UPLOAD heap.
	UINT constantBufferSize = (sizeof(LightingDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_lightingDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_lightingDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pLightingDataCbvDataBegin)));
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::CreateGridVertexBuffer(float radius)
{
	// The grid is generated procedurally on the CPU instead of being loaded from
	 // a file. It is only a teaching / orientation aid, so an UPLOAD heap is good
	 // enough and keeps the code shorter than a full default-heap upload path.
	constexpr int halfLineCount = 10;
	const float spacing = (std::max)(1.0f, ceilf((std::max)(radius, 1.0f) / static_cast<float>(halfLineCount)));
	const float extent = static_cast<float>(halfLineCount) * spacing;
	constexpr float gridY = 0.0f;
	const XMFLOAT3 minorColor(0.25f, 0.25f, 0.25f);
	const XMFLOAT3 xAxisColor(0.55f, 0.20f, 0.20f);
	const XMFLOAT3 zAxisColor(0.20f, 0.35f, 0.60f);

	std::vector<GridVertex> vertices;
	vertices.reserve(static_cast<size_t>((halfLineCount * 2 + 1) * 4));

	for (int i = -halfLineCount; i <= halfLineCount; ++i)
	{
		const float offset = static_cast<float>(i) * spacing;
		const XMFLOAT3 lineColor = (i == 0) ? xAxisColor : minorColor;
		vertices.push_back({ XMFLOAT3(-extent, gridY, offset), lineColor });
		vertices.push_back({ XMFLOAT3(extent, gridY, offset), lineColor });
	}

	for (int i = -halfLineCount; i <= halfLineCount; ++i)
	{
		const float offset = static_cast<float>(i) * spacing;
		const XMFLOAT3 lineColor = (i == 0) ? zAxisColor : minorColor;
		vertices.push_back({ XMFLOAT3(offset, gridY, -extent), lineColor });
		vertices.push_back({ XMFLOAT3(offset, gridY, extent), lineColor });
	}

	m_gridVertexCount = static_cast<UINT>(vertices.size());
	const UINT bufferSize = static_cast<UINT>(vertices.size() * sizeof(GridVertex));
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_gridVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_gridVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices.data(), bufferSize);
	m_gridVertexBuffer->Unmap(0, nullptr);

	m_gridVertexBufferView.BufferLocation = m_gridVertexBuffer->GetGPUVirtualAddress();
	m_gridVertexBufferView.StrideInBytes = sizeof(GridVertex);
	m_gridVertexBufferView.SizeInBytes = bufferSize;
}

void ZenithRenderEngine::LoadModelFromPath(const std::wstring& path)
{
	if (path.empty())
	{
		return;
	}

	m_renderContext->WaitForGpu();
	// Destroy the old model only after the GPU is idle. This is conservative but
	// easy to reason about while learning resource lifetime in D3D12.
	m_model.reset();
	m_modelOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);

	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
	// Model construction records upload commands for vertex/index/texture data.
	   // We open the upload command list first, then let Model fill it.
	m_renderContext->BeginUpload();
	auto commandList = m_renderContext->GetCommandList();
	auto model = std::make_unique<Model>(cbvSrvUavAllocator, device, commandList, WideToUtf8(path));
	m_renderContext->EndUpload();

	if (!model->IsLoaded())
	{
		MessageBoxW(Win32Application::GetHwnd(), L"Failed to load the selected model.", GetTitle(), MB_OK | MB_ICONERROR);
		return;
	}

	const XMFLOAT3 boundsCenter = model->GetBoundsCenter();
	const XMFLOAT3 boundsMin = model->GetBoundsMin();
	const float boundsRadius = model->GetBoundsRadius();
	// The model is recentered so that:
	 //   - X/Z are centered around the origin
	 //   - the lowest Y point sits on the ground plane
	 // This makes orbiting and grid visualization feel much nicer.
	m_modelOffset = XMFLOAT3(-boundsCenter.x, -boundsMin.y, -boundsCenter.z);
	m_camera.FrameBoundingSphere(XMFLOAT3(0.0f, boundsCenter.y - boundsMin.y, 0.0f), boundsRadius);
	CreateGridVertexBuffer(boundsRadius);
	model->ReleaseUploadBuffers();
	m_model = std::move(model);
	SetCustomWindowText(path.c_str());
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	// Update camera (smoothing / spring hook – currently instant)
	m_camera.Update();

	// Build the matrices that shaders need this frame.
	  //
	  // In this sample the world matrix is only a translation used to recenter the
	  // imported model, but this is exactly where rotation / scale / animation would
	  // be composed in a larger renderer.
	const XMMATRIX translate = XMMatrixTranslation(
		m_modelOffset.x,
		m_modelOffset.y,
		m_modelOffset.z);
	XMMATRIX world = translate;

	const XMMATRIX view = m_camera.GetViewMatrix();
	const XMMATRIX proj = m_camera.GetProjectionMatrix();
	const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	// HLSL constant buffers are treated as column-major here, so the matrices are
	  // transposed before upload to match shader-side multiplication.
	XMStoreFloat4x4(&m_sceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_sceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_sceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_sceneDataCbData.normalMatrix, normalMat);

	XMStoreFloat4(&m_lightingDataCbData.viewPosition, m_camera.GetPosition());

	m_lightingDataCbData.directionalLight.direction = XMFLOAT4(-0.2f, -1.0f, -0.3f, 1.0f);
	if (m_directionalLightEnabled)
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.2f, 0.2f, 0.2f, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
	}
	else
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	// Because both constant buffers live in persistently mapped UPLOAD heaps,
	// updating them is just a memcpy into the mapped CPU pointer.
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	std::wstring capturePath = std::move(m_pendingRenderImagePath);
	m_pendingRenderImagePath.clear();

	// 1. Prepare the command list for rendering (Reset command allocator, command list, set render target, etc.)
	m_renderContext->Prepare();

	auto commandList = m_renderContext->GetCommandList();

	// 2. Bind the root signature and create this frame's temporary CBV descriptors.
	  //
	  // The descriptor allocator already copied all static descriptors (mainly model
	  // textures) into the current shader-visible heap during Prepare().
	  // Now we append temporary per-frame descriptors after that static range.
	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
	const UINT sceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT lightingDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	D3D12_CONSTANT_BUFFER_VIEW_DESC sceneCbvDesc = {};
	sceneCbvDesc.BufferLocation = m_sceneDataConstantBuffer->GetGPUVirtualAddress();
	sceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&sceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(sceneDescriptorIndex));
	D3D12_CONSTANT_BUFFER_VIEW_DESC lightingCbvDesc = {};
	lightingCbvDesc.BufferLocation = m_lightingDataConstantBuffer->GetGPUVirtualAddress();
	lightingCbvDesc.SizeInBytes = (sizeof(LightingDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&lightingCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(lightingDescriptorIndex));

	// Bind the single shader-visible CBV/SRV/UAV heap for this frame, then bind
	 // root tables by index into that heap.
	ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavAllocator->GetShaderVisibleHeap() };
	commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	commandList->SetGraphicsRootDescriptorTable(0, cbvSrvUavAllocator->GetDynamicGpuHandle(0));
	commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	commandList->SetGraphicsRootDescriptorTable(3, cbvSrvUavAllocator->GetDynamicGpuHandle(lightingDescriptorIndex));

	// 3. At this point all root bindings needed by the shaders are in place:
	//   slot 0 -> texture SRV table
	//   slot 1 -> scene CBV table
	//   slot 2 -> material root CBV (set later per mesh)
	//   slot 3 -> lighting CBV table

	// 4. Set up scissor rect and viewport
	commandList->RSSetViewports(1, &m_viewport);
	commandList->RSSetScissorRects(1, &m_scissorRect);

	// 5. Select the current back buffer and depth buffer, then clear them.
	   // The RTV handle is offset by the current swap-chain frame index because each
	   // back buffer has its own render-target descriptor.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		m_renderContext->GetRtvHeapStart(), // Bạn cần thêm Getter này trong RenderContext
		m_renderContext->GetCurrentFrameIndex(),
		m_renderContext->GetRtvDescriptorSize()
	);
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_renderContext->GetDsvHeapStart());

	const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// Draw grid first so the viewer has visual orientation even before a model is loaded.
	commandList->SetPipelineState(m_gridPipelineState.Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
	commandList->IASetVertexBuffers(0, 1, &m_gridVertexBufferView);
	commandList->DrawInstanced(m_gridVertexCount, 1, 0, 0);

	// Switch to the main model PSO for triangle rendering.
	commandList->SetPipelineState(m_pipelineState.Get());

	// 6. Draw the model
	if (m_model) {
		m_model->Draw(commandList);
	}

	// 7. Present() closes and executes the command list, then presents the swap chain.
	const bool captureSucceeded = m_renderContext->Present(capturePath);
	if (!capturePath.empty() && !captureSucceeded)
	{
		MessageBoxW(
			Win32Application::GetHwnd(),
			L"Failed to save the rendered image.",
			GetTitle(),
			MB_OK | MB_ICONERROR);
	}
}

void ZenithRenderEngine::OnDestroy()
{
	// Make sure to wait for the GPU to finish all pending work before exiting to avoid access violations and ensure proper cleanup of resources.
	m_renderContext->WaitForGpu();
}

// ---------------------------------------------------------------------------
// Mouse handlers – delegate straight to the Camera
// ---------------------------------------------------------------------------

void ZenithRenderEngine::OnMiddleButtonDown(int x, int y)
{
	m_camera.OnMiddleButtonDown(x, y);
}

void ZenithRenderEngine::OnMiddleButtonUp(int x, int y)
{
	m_camera.OnMiddleButtonUp();
}

void ZenithRenderEngine::OnMouseMove(int x, int y, WPARAM btnState)
{
	// Shift held = pan, otherwise orbit
	const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	m_camera.OnMouseMove(x, y, shiftDown);
}

void ZenithRenderEngine::OnMouseWheel(float wheelDelta)
{
	m_camera.OnMouseWheel(wheelDelta);
}