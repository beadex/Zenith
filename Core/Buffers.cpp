#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"
#include "Shared.h"

using namespace RenderEngineDetail;

void ZenithRenderEngine::CreateSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_sceneDataConstantBuffer)));

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

void ZenithRenderEngine::CreatePointShadowSceneDataConstantBuffers()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pointShadowSceneDataConstantBuffers[faceIndex])));

		CD3DX12_RANGE readRange(0, 0);
		ThrowIfFailed(m_pointShadowSceneDataConstantBuffers[faceIndex]->Map(
			0,
			&readRange,
			reinterpret_cast<void**>(&m_pPointShadowSceneDataCbvDataBegin[faceIndex])));
		memcpy(
			m_pPointShadowSceneDataCbvDataBegin[faceIndex],
			&m_pointShadowSceneDataCbData[faceIndex],
			sizeof(m_pointShadowSceneDataCbData[faceIndex]));
	}
}

void ZenithRenderEngine::CreateLightingDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
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

void ZenithRenderEngine::CreatePointLightGizmoVertexBuffer()
{
	const XMFLOAT3 color = m_isPointLightHovered ? MakePointLightHoverColor(m_pointLightColor) : m_pointLightColor;
	const GridVertex vertices[] = {
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color }
	};

	m_pointLightGizmoVertexCount = _countof(vertices);
	const UINT bufferSize = sizeof(vertices);
	auto device = m_renderContext->GetDevice();

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_pointLightGizmoVertexBuffer)));

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightGizmoVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, bufferSize);
	m_pointLightGizmoVertexBuffer->Unmap(0, nullptr);

	m_pointLightGizmoVertexBufferView.BufferLocation = m_pointLightGizmoVertexBuffer->GetGPUVirtualAddress();
	m_pointLightGizmoVertexBufferView.StrideInBytes = sizeof(GridVertex);
	m_pointLightGizmoVertexBufferView.SizeInBytes = bufferSize;
	UpdatePointLightGizmoVertexBuffer();
}

void ZenithRenderEngine::UpdatePointLightGizmoVertexBuffer()
{
	if (!m_pointLightGizmoVertexBuffer)
	{
		return;
	}

	const XMFLOAT3 color = m_isPointLightHovered ? MakePointLightHoverColor(m_pointLightColor) : m_pointLightColor;
	const GridVertex vertices[] = {
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f, -0.5f,  0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(0.5f,  0.5f,  0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f, -0.5f), color },
		{ XMFLOAT3(-0.5f,  0.5f,  0.5f), color }
	};

	UINT8* mappedData = nullptr;
	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightGizmoVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
	memcpy(mappedData, vertices, sizeof(vertices));
	m_pointLightGizmoVertexBuffer->Unmap(0, nullptr);
}

void ZenithRenderEngine::CreatePointLightSceneDataConstantBuffer()
{
	auto device = m_renderContext->GetDevice();
	const UINT constantBufferSize = (sizeof(SceneDataConstantBuffer) + 255) & ~255;

	ThrowIfFailed(device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_pointLightSceneDataConstantBuffer)));

	CD3DX12_RANGE readRange(0, 0);
	ThrowIfFailed(m_pointLightSceneDataConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_pPointLightSceneDataCbvDataBegin)));
	memcpy(m_pPointLightSceneDataCbvDataBegin, &m_pointLightSceneDataCbData, sizeof(m_pointLightSceneDataCbData));
}

void ZenithRenderEngine::CreateGroundPlaneVertexBuffer(float radius)
{
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
	m_model.reset();
	m_modelOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);

	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
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
	m_modelOffset = XMFLOAT3(-boundsCenter.x, -boundsMin.y, -boundsCenter.z);
	m_camera.FrameBoundingSphere(XMFLOAT3(0.0f, boundsCenter.y - boundsMin.y, 0.0f), boundsRadius);
	CreateGridVertexBuffer(boundsRadius);
	CreateGroundPlaneVertexBuffer(boundsRadius);
	model->ReleaseUploadBuffers();
	m_model = std::move(model);
	SetCustomWindowText(path.c_str());
}
