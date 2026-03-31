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
//
// Shadow features added so far in this file:
//   1. create depth-only PSOs for rendering from the light
//   2. build a light view/projection every frame
//   3. fit the orthographic shadow frustum to the model bounds
//   4. render a shadow pass before the main pass
//   5. provide an optional solid ground plane to inspect shadow quality
//
// So when reading this file from top to bottom, the beginner flow is:
//   - setup GPU states and buffers in `OnInit()`
//   - compute camera + light transforms in `OnUpdate()`
//   - render shadow map first in `OnRender()`
//   - render the normal shaded scene second in `OnRender()`
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
		 //
		 // For the shadow work added so far, this stage creates:
		 //   - main scene PSOs
		 //   - shadow-only PSOs
		 //   - constant buffers for the camera pass, light pass, and inspection plane
		 //   - helper geometry like the line grid and optional solid ground plane
	CreateRootSignature();
	// The renderer now creates four model PSOs so meshes can be dispatched by the
	  // combination of transparency and double-sided state.
	CreatePipelineState();
	CreateDoubleSidedPipelineState();
	CreateTransparentPipelineState();
	CreateDoubleSidedTransparentPipelineState();
	CreateGridPipelineState();
	CreateSceneDataConstantBuffer();
	// The shadow pass needs its own SceneData buffer because its camera is the light,
	  // not the user's view camera.
	CreateShadowSceneDataConstantBuffer();
	// The optional inspection plane also gets its own SceneData because it does not
	   // use the model's translated world transform.
	CreateGroundPlaneSceneDataConstantBuffer();
	CreateLightingDataConstantBuffer();
	// Shadow PSOs are depth-only render states used before the main color pass.
	CreateShadowPipelineState();
	CreateDoubleSidedShadowPipelineState();
	CreateGridVertexBuffer();
	// This plane is only for debugging / viewing shadows better.
	CreateGroundPlaneVertexBuffer();
	CreateGroundPlaneMaterialConstantBuffer();
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
	case IDM_VIEW_SOLID_GROUND_PLANE:
		// Swap the visual helper from a wire grid to a solid plane.
		   // The plane makes it much easier to judge whether shadows look correct.
		m_useSolidGroundPlane = !m_useSolidGroundPlane;
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
	CheckMenuItem(
		menu,
		IDM_VIEW_SOLID_GROUND_PLANE,
		MF_BYCOMMAND | (m_useSolidGroundPlane ? MF_CHECKED : MF_UNCHECKED));
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
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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

	// Main PSO: opaque + single-sided.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateShadowPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.DepthBias = ShadowRasterDepthBias;
	psoDesc.RasterizerState.SlopeScaledDepthBias = ShadowRasterSlopeScaledDepthBias;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	// This PSO has no pixel shader and no render targets because the shadow pass
	 // only writes depth into the shadow map.
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	// A small depth bias helps reduce "shadow acne" caused by limited depth precision.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedShadowPipelineState()
{
	auto device = m_renderContext->GetDevice();
	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowVSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_ShadowPSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	// Same idea as the main shadow PSO, but culling is disabled for double-sided meshes.
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.DepthBias = ShadowRasterDepthBias;
	psoDesc.RasterizerState.SlopeScaledDepthBias = ShadowRasterSlopeScaledDepthBias;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedShadowPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateTransparentPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

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
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	auto& transparentBlend = psoDesc.BlendState.RenderTarget[0];
	transparentBlend.BlendEnable = TRUE;
	transparentBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparentBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOp = D3D12_BLEND_OP_ADD;
	transparentBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparentBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparentBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Transparent PSO: transparent + single-sided.
	// Depth testing stays enabled, but depth writes are disabled so multiple
	// transparent layers can still contribute when drawn back-to-front.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_transparentPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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

	// Double-sided opaque PSO: same as the main opaque PSO except culling is off.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedPipelineState)));

	free(pVertexShaderData);
	free(pPixelShaderData);
}

void ZenithRenderEngine::CreateDoubleSidedTransparentPipelineState()
{
	auto device = m_renderContext->GetDevice();

	UINT8* pVertexShaderData = nullptr;
	UINT8* pPixelShaderData = nullptr;
	UINT vertexShaderDataLength = 0;
	UINT pixelShaderDataLength = 0;

	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_VSMain.cso").c_str(), &pVertexShaderData, &vertexShaderDataLength));
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(L"shaders_PSMain.cso").c_str(), &pPixelShaderData, &pixelShaderDataLength));

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;

	auto& transparentBlend = psoDesc.BlendState.RenderTarget[0];
	transparentBlend.BlendEnable = TRUE;
	transparentBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparentBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOp = D3D12_BLEND_OP_ADD;
	transparentBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparentBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	transparentBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparentBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Double-sided transparent PSO: transparent + no culling.
	ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_doubleSidedTransparentPipelineState)));

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

void ZenithRenderEngine::CreateShadowSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_shadowSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_shadowSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pShadowSceneDataCbvDataBegin)));
	memcpy(m_pShadowSceneDataCbvDataBegin, &m_shadowSceneDataCbData, sizeof(m_shadowSceneDataCbData));
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

void ZenithRenderEngine::CreateGroundPlaneVertexBuffer(float radius)
{
	// Build a simple two-triangle quad at y = 0 so shadows have a solid receiver.
	const float extent = (std::max)(10.0f, radius * 1.5f);
	const Vertex vertices[] = {
		{ XMFLOAT3(-extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) },
		{ XMFLOAT3(-extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) },
		{ XMFLOAT3(extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) },
		{ XMFLOAT3(-extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) },
		{ XMFLOAT3(extent, 0.0f,  extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) },
		{ XMFLOAT3(extent, 0.0f, -extent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) },
	};

	m_groundPlaneVertexCount = _countof(vertices);
	const UINT bufferSize = sizeof(vertices);
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, bufferSize);
	m_groundPlaneVertexBuffer->Unmap(0, nullptr);

	m_groundPlaneVertexBufferView.BufferLocation = m_groundPlaneVertexBuffer->GetGPUVirtualAddress();
	m_groundPlaneVertexBufferView.StrideInBytes = sizeof(Vertex);
	m_groundPlaneVertexBufferView.SizeInBytes = bufferSize;
}

void ZenithRenderEngine::CreateGroundPlaneSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pGroundPlaneSceneDataCbvDataBegin)));
	memcpy(m_pGroundPlaneSceneDataCbvDataBegin, &m_groundPlaneSceneDataCbData, sizeof(m_groundPlaneSceneDataCbData));
}

void ZenithRenderEngine::CreateGroundPlaneMaterialConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(MaterialData) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_groundPlaneMaterialConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_groundPlaneMaterialConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pGroundPlaneMaterialCbvDataBegin)));

	// The plane is intentionally simple: no textures, just a constant gray tint.
	m_groundPlaneMaterialData = {};
	m_groundPlaneMaterialData.alphaMode = 0;
	m_groundPlaneMaterialData.baseColorFactor = XMFLOAT4(0.55f, 0.55f, 0.55f, 1.0f);
	memcpy(m_pGroundPlaneMaterialCbvDataBegin, &m_groundPlaneMaterialData, sizeof(m_groundPlaneMaterialData));
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
	CreateGroundPlaneVertexBuffer(boundsRadius);
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
		 //
		 // This function is also where all shadow-camera work is prepared:
		 //   - choose a light direction
		 //   - position a light camera
		 //   - fit an orthographic projection to the model bounds
		 //   - upload the final lightViewProjection matrix for the main shader
	const XMMATRIX translate = XMMatrixTranslation(
		m_modelOffset.x,
		m_modelOffset.y,
		m_modelOffset.z);
	XMMATRIX world = translate;

	const XMMATRIX view = m_camera.GetViewMatrix();
	const XMMATRIX proj = m_camera.GetProjectionMatrix();
	const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
	// Build a simple directional-light camera for shadow mapping.
	 //
	 // Think of this as placing a second camera in the world:
	 //   - the normal camera renders what the player sees
	 //   - the light camera renders what the light sees
	const XMFLOAT3 directionalLightDirection = XMFLOAT3(-0.2f, -1.0f, -0.3f);

	XMFLOAT3 shadowTarget = XMFLOAT3(0.0f, 0.0f, 0.0f);
	float shadowBoundsRadius = 15.0f;
	XMFLOAT3 shadowBoundsMin = XMFLOAT3(-shadowBoundsRadius, 0.0f, -shadowBoundsRadius);
	XMFLOAT3 shadowBoundsMax = XMFLOAT3(shadowBoundsRadius, shadowBoundsRadius * 2.0f, shadowBoundsRadius);
	if (m_model)
	{
		const XMFLOAT3 boundsCenter = m_model->GetBoundsCenter();
		const XMFLOAT3 boundsMin = m_model->GetBoundsMin();
		const XMFLOAT3 boundsMax = m_model->GetBoundsMax();
		shadowTarget = XMFLOAT3(0.0f, boundsCenter.y - boundsMin.y, 0.0f);
		shadowBoundsRadius = (std::max)(m_model->GetBoundsRadius(), 1.0f);
		shadowBoundsMin = XMFLOAT3(
			boundsMin.x + m_modelOffset.x,
			boundsMin.y + m_modelOffset.y,
			boundsMin.z + m_modelOffset.z);
		shadowBoundsMax = XMFLOAT3(
			boundsMax.x + m_modelOffset.x,
			boundsMax.y + m_modelOffset.y,
			boundsMax.z + m_modelOffset.z);
	}

	const XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&directionalLightDirection));
	const XMVECTOR shadowTargetVector = XMLoadFloat3(&shadowTarget);
	XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
	if (fabsf(XMVectorGetX(XMVector3Dot(lightDirection, lightUp))) > 0.99f)
	{
		lightUp = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
	}

	const XMVECTOR lightPosition = shadowTargetVector - lightDirection * (shadowBoundsRadius * 2.5f);
	const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, shadowTargetVector, lightUp);

	// Fit the orthographic shadow camera to the model's transformed bounds in light space.
	   // This uses the shadow-map texels more efficiently than a loose radius-based box.
	   //
	   // Why do this?
	   //   - a loose box wastes shadow resolution on empty space
	   //   - a tighter box gives sharper shadows from the same texture size
	   //
	   // The idea is simple:
	   //   1. build the 8 corners of the model's world-space bounding box
	   //   2. transform those corners into light space
	   //   3. compute min/max extents there
	   //   4. build an off-center orthographic projection that tightly encloses them
	const XMFLOAT3 shadowCorners[] = {
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMin.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMin.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMax.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMin.x, shadowBoundsMax.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMin.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMin.y, shadowBoundsMax.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMax.y, shadowBoundsMin.z),
		XMFLOAT3(shadowBoundsMax.x, shadowBoundsMax.y, shadowBoundsMax.z)
	};

	float minLightX = FLT_MAX;
	float minLightY = FLT_MAX;
	float minLightZ = FLT_MAX;
	float maxLightX = -FLT_MAX;
	float maxLightY = -FLT_MAX;
	float maxLightZ = -FLT_MAX;
	for (const XMFLOAT3& corner : shadowCorners)
	{
		const XMVECTOR lightSpaceCorner = XMVector3TransformCoord(XMLoadFloat3(&corner), lightView);
		const float lightX = XMVectorGetX(lightSpaceCorner);
		const float lightY = XMVectorGetY(lightSpaceCorner);
		const float lightZ = XMVectorGetZ(lightSpaceCorner);
		minLightX = (std::min)(minLightX, lightX);
		minLightY = (std::min)(minLightY, lightY);
		minLightZ = (std::min)(minLightZ, lightZ);
		maxLightX = (std::max)(maxLightX, lightX);
		maxLightY = (std::max)(maxLightY, lightY);
		maxLightZ = (std::max)(maxLightZ, lightZ);
	}

	const float xyPadding = (std::max)(ShadowMinFrustumPadding, shadowBoundsRadius * 0.15f);
	const float zPadding = (std::max)(ShadowMinFrustumPadding * 4.0f, shadowBoundsRadius * ShadowDepthRangePaddingScale);
	const float nearPlane = (std::max)(0.1f, minLightZ - zPadding);
	const float farPlane = (std::max)(nearPlane + 1.0f, maxLightZ + zPadding);
	const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
		minLightX - xyPadding,
		maxLightX + xyPadding,
		minLightY - xyPadding,
		maxLightY + xyPadding,
		nearPlane,
		farPlane);
	const XMMATRIX lightViewProjection = lightView * lightProjection;

	// HLSL constant buffers are treated as column-major here, so the matrices are
	  // transposed before upload to match shader-side multiplication.
	XMStoreFloat4x4(&m_sceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_sceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_sceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_sceneDataCbData.normalMatrix, normalMat);
	// The ground plane stays at the origin instead of using the model recenter offset.
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.model, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.normalMatrix, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.view, XMMatrixTranspose(lightView));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.projection, XMMatrixTranspose(lightProjection));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.normalMatrix, normalMat);

	XMStoreFloat4(&m_lightingDataCbData.viewPosition, m_camera.GetPosition());

	m_lightingDataCbData.directionalLight.direction = XMFLOAT4(directionalLightDirection.x, directionalLightDirection.y, directionalLightDirection.z, 1.0f);
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

	// This matrix is the bridge between the main pass and the shadow map.
	   // Each main-pass pixel transforms itself into this light space to test shadowing.
	XMStoreFloat4x4(&m_lightingDataCbData.lightViewProjection, XMMatrixTranspose(lightViewProjection));
	m_lightingDataCbData.shadowParams = XMFLOAT4(
		static_cast<float>(m_renderContext->GetShadowMapSrvIndex()),
		ShadowComparisonBias,
		FlipNormalMapGreenChannel ? -1.0f : 1.0f,
		m_directionalLightEnabled ? 1.0f : 0.0f);

	// Because both constant buffers live in persistently mapped UPLOAD heaps,
	// updating them is just a memcpy into the mapped CPU pointer.
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
	memcpy(m_pGroundPlaneSceneDataCbvDataBegin, &m_groundPlaneSceneDataCbData, sizeof(m_groundPlaneSceneDataCbData));
	memcpy(m_pShadowSceneDataCbvDataBegin, &m_shadowSceneDataCbData, sizeof(m_shadowSceneDataCbData));
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	std::wstring capturePath = std::move(m_pendingRenderImagePath);
	m_pendingRenderImagePath.clear();

	// 1. Prepare the command list for rendering (Reset command allocator, command list, set render target, etc.)
	 //
	 // Important rendering order for the whole shadow system:
	 //   1. prepare command list and per-frame descriptors
	 //   2. render shadow map from the light's point of view
	 //   3. render the main scene from the camera's point of view
	 //   4. present the back buffer
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
	const UINT shadowSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT groundPlaneSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT sceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT lightingDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	// The shadow pass uses its own SceneData descriptor because it renders from the light.
	D3D12_CONSTANT_BUFFER_VIEW_DESC shadowSceneCbvDesc = {};
	shadowSceneCbvDesc.BufferLocation = m_shadowSceneDataConstantBuffer->GetGPUVirtualAddress();
	shadowSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&shadowSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(shadowSceneDescriptorIndex));
	// The ground plane also has separate SceneData because its world transform differs
	 // from the model's transform.
	D3D12_CONSTANT_BUFFER_VIEW_DESC groundPlaneSceneCbvDesc = {};
	groundPlaneSceneCbvDesc.BufferLocation = m_groundPlaneSceneDataConstantBuffer->GetGPUVirtualAddress();
	groundPlaneSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&groundPlaneSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(groundPlaneSceneDescriptorIndex));
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

	// 3. At this point all root bindings needed by the shaders are in place:
	 //   slot 0 -> texture SRV table
	 //   slot 1 -> scene CBV table
	 //   slot 2 -> material root CBV (set later per mesh)
	 //   slot 3 -> lighting CBV table
	 //
	 // The shadow pass reuses the same root signature layout. The only difference is
	 // that slot 1 points at the light-camera SceneData instead of the normal camera.
	if (m_model)
	{
		// --- Shadow pass ---
		   // 1. Turn the shadow map from a sampled texture into a writable depth target.
		auto shadowToDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		commandList->ResourceBarrier(1, &shadowToDepthWrite);

		const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle = m_renderContext->GetShadowMapDsv();
		// 2. Bind the light's camera matrices, shadow viewport, and shadow DSV.
		commandList->SetGraphicsRootSignature(m_rootSignature.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(shadowSceneDescriptorIndex));
		commandList->RSSetViewports(1, &m_renderContext->GetShadowMapViewport());
		commandList->RSSetScissorRects(1, &m_renderContext->GetShadowMapScissorRect());
		commandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);

		// 3. Render geometry from the light's point of view.
		//
		// The shadow PSO is depth-focused, so no color is written here.
		// Instead, the GPU stores the nearest surface depth into the shadow map.
		// Later, the main shader compares its current pixel depth against that value.
		commandList->SetPipelineState(m_shadowPipelineState.Get());
		m_model->DrawOpaque(commandList, false);
		commandList->SetPipelineState(m_doubleSidedShadowPipelineState.Get());
		m_model->DrawOpaque(commandList, true);

		// 4. Turn the shadow map back into a shader-readable texture for the main pass.
		auto shadowToPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &shadowToPixelShaderResource);
	}

	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	commandList->SetGraphicsRootDescriptorTable(3, cbvSrvUavAllocator->GetDynamicGpuHandle(lightingDescriptorIndex));

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

	if (m_useSolidGroundPlane)
	{
		// Draw the debugging ground plane with the normal lit shader so it receives shadows.
		commandList->SetPipelineState(m_doubleSidedPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(groundPlaneSceneDescriptorIndex));
		commandList->SetGraphicsRootConstantBufferView(2, m_groundPlaneMaterialConstantBuffer->GetGPUVirtualAddress());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->IASetVertexBuffers(0, 1, &m_groundPlaneVertexBufferView);
		commandList->DrawInstanced(m_groundPlaneVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}
	else
	{
		// Draw grid first so the viewer has visual orientation even before a model is loaded.
		commandList->SetPipelineState(m_gridPipelineState.Get());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		commandList->IASetVertexBuffers(0, 1, &m_gridVertexBufferView);
		commandList->DrawInstanced(m_gridVertexCount, 1, 0, 0);
	}

	// Opaque meshes render first so they fill depth normally.
	  // At this point the shadow map is already ready for sampling in the pixel shader.
	commandList->SetPipelineState(m_pipelineState.Get());

	// 6. Draw the model
	if (m_model) {
		// Draw order matters:
		//   1. opaque single-sided
		//   2. opaque double-sided
		//   3. transparent single-sided
		//   4. transparent double-sided
		//
		// This keeps the common opaque path fast and predictable while still
		// honoring glTF doubleSided on the meshes that actually request it.
		m_model->DrawOpaque(commandList, false);
		commandList->SetPipelineState(m_doubleSidedPipelineState.Get());
		m_model->DrawOpaque(commandList, true);

		XMFLOAT3 cameraPosition;
		XMStoreFloat3(&cameraPosition, m_camera.GetPosition());
		commandList->SetPipelineState(m_transparentPipelineState.Get());
		m_model->DrawTransparent(commandList, cameraPosition, m_modelOffset, false);
		commandList->SetPipelineState(m_doubleSidedTransparentPipelineState.Get());
		m_model->DrawTransparent(commandList, cameraPosition, m_modelOffset, true);
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