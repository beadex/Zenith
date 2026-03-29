#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"

ZenithRenderEngine::ZenithRenderEngine(UINT width, UINT height, std::wstring name) :
	D3D12Application(width, height, name),
	m_model(nullptr),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
	m_cameraPos(XMVectorSet(2.2f, 1.6f, 7.0f, 1.0f)),
	m_cameraFront(XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f)),
	m_cameraUp(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)),
	m_cbData{},
	m_pCbvDataBegin(nullptr)
{
}

void ZenithRenderEngine::OnInit()
{
	// 1. Initialize D3D12 Context (Device, Swap Chain, Command Queue, RTV Heap...)
	m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

	// 2. Load Assets (Shader, Geometry...)
	CreateRootSignature();
	CreatePipelineState();

	// Load Model
	auto device = m_renderContext->GetDevice();

	m_renderContext->BeginUpload();
	auto commandList = m_renderContext->GetCommandList();
	m_model = std::make_unique<Model>(device, commandList, "Assets/Models/cnek.3ds");
	m_renderContext->EndUpload();

	CreateConstantBuffer();
}

void ZenithRenderEngine::CreateRootSignature()
{
	auto device = m_renderContext->GetDevice();

	// 1. Tạo Root Signature
	CD3DX12_ROOT_PARAMETER rootParameters[1];
	rootParameters[0].InitAsConstantBufferView(0); // register(b0) cho MVP matrix

	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
	rootSigDesc.Init(_countof(rootParameters), rootParameters, 0, nullptr,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature;
	ComPtr<ID3DBlob> error;
	ThrowIfFailed(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
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
}

void ZenithRenderEngine::CreateConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	// Kích thước Constant Buffer phải là bội số của 256 bytes
	UINT constantBufferSize = (sizeof(SceneConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_constantBuffer)));

	// Map vùng nhớ để CPU có thể ghi dữ liệu vào m_pCbvDataBegin
	CD3DX12_RANGE readRange(0, 0); // Chúng ta không đọc từ GPU
	ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pCbvDataBegin)));
	memcpy(m_pCbvDataBegin, &m_cbData, sizeof(m_cbData));
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	float angle = timer.TotalTime();

	XMMATRIX scale = XMMatrixScaling(0.01f, 0.01f, 0.01f);
	XMMATRIX world = scale * XMMatrixRotationY(angle);
	XMMATRIX view = XMMatrixLookAtLH(
		m_cameraPos,  // Eye position
		m_cameraPos + m_cameraFront,    // Look-at target
		m_cameraUp     // Up direction
	);
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, m_aspectRatio, 0.5f, 50.0f);

	XMMATRIX mvp = world * view * proj;
	// Transpose because HLSL expects column-major by default
	XMStoreFloat4x4(&m_cbData.mvp, XMMatrixTranspose(mvp));
	memcpy(m_pCbvDataBegin, &m_cbData, sizeof(m_cbData));
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
	commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

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