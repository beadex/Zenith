#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"

#pragma comment(lib, "Comdlg32.lib")

namespace
{
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

ZenithRenderEngine::ZenithRenderEngine(UINT width, UINT height, std::wstring name) :
	D3D12Application(width, height, name),
	m_model(nullptr),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
	m_cameraPos(XMVectorSet(0.0f, 0.0f, 8.0f, 1.0f)),
	m_cameraFront(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f)),
	m_cameraUp(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)),
	m_sceneDataCbData{},
	m_pSceneDataCbvDataBegin(nullptr)
{
}

void ZenithRenderEngine::OnInit()
{
	// 1. Initialize D3D12 Context (Device, Swap Chain, Command Queue, RTV Heap...)
	m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

	// 2. Load Assets (Shader, Geometry...)
	CreateRootSignature();
	CreatePipelineState();
	CreateSceneDataConstantBuffer();
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
	default:
		return false;
	}
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

	CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

	// 1. Tạo Root Signature
	CD3DX12_ROOT_PARAMETER1 rootParameters[3];
	// Root param 0: SRV heap (texture array) → pixel shader
	rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

	// Root param 1: SceneData b0 → vertex shader (CBV trực tiếp, không qua table)
	rootParameters[1].InitAsConstantBufferView(0, 0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_VERTEX);

	// Root param 2: MaterialData b1 → pixel shader (CBV trực tiếp, không qua table)
	rootParameters[2].InitAsConstantBufferView(1, 0,
		D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_PIXEL);

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

	// 3. Cấu hình PSO
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

	delete[] pVertexShaderData;
	delete[] pPixelShaderData;
}

void ZenithRenderEngine::CreateSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// Kích thước Constant Buffer phải là bội số của 256 bytes
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_sceneDataConstantBuffer)));

	// Map vùng nhớ để CPU có thể ghi dữ liệu vào m_pCbvDataBegin
	CD3DX12_RANGE readRange(0, 0); // Chúng ta không đọc từ GPU
	ThrowIfFailed(m_sceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pSceneDataCbvDataBegin)));
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
}

void ZenithRenderEngine::LoadModelFromPath(const std::wstring& path)
{
	if (path.empty())
	{
		return;
	}

	m_renderContext->WaitForGpu();
	m_model.reset();

	auto device = m_renderContext->GetDevice();
	m_renderContext->BeginUpload();
	auto commandList = m_renderContext->GetCommandList();
	auto model = std::make_unique<Model>(device, commandList, WideToUtf8(path));
	m_renderContext->EndUpload();

	if (!model->IsLoaded())
	{
		MessageBoxW(Win32Application::GetHwnd(), L"Failed to load the selected model.", GetTitle(), MB_OK | MB_ICONERROR);
		return;
	}

	model->ReleaseUploadBuffers();
	m_model = std::move(model);
	SetCustomWindowText(path.c_str());
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	float angle = timer.TotalTime();

	XMMATRIX scale = XMMatrixScaling(0.5f, 0.5f, 0.5f); // ← thêm lại
	XMMATRIX world = scale * XMMatrixRotationY(angle);
	XMMATRIX view = XMMatrixLookAtLH(
		m_cameraPos,  // Eye position
		m_cameraPos + m_cameraFront,    // Look-at target
		m_cameraUp     // Up direction
	);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, m_aspectRatio, 0.5f, 50.0f);
	const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	// Transpose because HLSL expects column-major by default
	XMStoreFloat4x4(&m_sceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_sceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_sceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_sceneDataCbData.normalMatrix, normalMatrix);
	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	// 1. Prepare the command list for rendering (Reset command allocator, command list, set render target, etc.)
	m_renderContext->Prepare();

	auto commandList = m_renderContext->GetCommandList();

	// 2. Set the root signature and pipeline state for rendering
	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	commandList->SetPipelineState(m_pipelineState.Get());

	// 3. Update constant buffer with the latest transformation matrices (Model-View-Projection)
	commandList->SetGraphicsRootConstantBufferView(1, m_sceneDataConstantBuffer->GetGPUVirtualAddress());

	// 4. Set up scissor rect and viewport
	commandList->RSSetViewports(1, &m_viewport);
	commandList->RSSetScissorRects(1, &m_scissorRect);

	// 5. Record commands to clear the render target and set it for drawing. You can also set the viewport and scissor rect here if needed.
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		m_renderContext->GetRtvHeapStart(), // Bạn cần thêm Getter này trong RenderContext
		m_renderContext->GetCurrentFrameIndex(),
		m_renderContext->GetRtvDescriptorSize()
	);
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_renderContext->GetDsvHeapStart());

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	// 6. Draw the model
	if (m_model) {
		m_model->Draw(commandList);
	}

	// 7. Record commands to draw geometry here (Set pipeline state, set root signature, set vertex/index buffers, draw calls, etc.)
	m_renderContext->Present();
}

void ZenithRenderEngine::OnDestroy()
{
	// Make sure to wait for the GPU to finish all pending work before exiting to avoid access violations and ensure proper cleanup of resources.
	m_renderContext->WaitForGpu();
}