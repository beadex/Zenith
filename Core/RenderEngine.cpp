#include "pch.h"
#include "RenderEngine.h"
#include "Win32Application.h"
#include "D3D12ApplicationHelper.h"
#include "DescriptorAllocator.h"
#include "Shared.h"

using namespace RenderEngineDetail;

// ---------------------------------------------------------------------------
// ZenithRenderEngine
//
// This file is the "high-level renderer" entry point for the sample. It keeps
// the constructor and the main frame lifecycle together so a reader can still
// follow the top-level rendering flow without digging through every subsystem.
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
	m_camera.SetLens(XM_PIDIV4,
		static_cast<float>(width) / static_cast<float>(height),
		0.1f, 500.0f);
}

void ZenithRenderEngine::OnInit()
{
	INITCOMMONCONTROLSEX commonControls = {};
	commonControls.dwSize = sizeof(commonControls);
	commonControls.dwICC = ICC_BAR_CLASSES;
	InitCommonControlsEx(&commonControls);

	m_renderContext->Initialize(Win32Application::GetHwnd(), m_useWarpDevice);

	CreateRootSignature();
	CreatePipelineState();
	CreateDoubleSidedPipelineState();
	CreateTransparentPipelineState();
	CreateDoubleSidedTransparentPipelineState();
	CreateGridPipelineState();
	CreatePointLightGizmoPipelineState();
	CreateSceneDataConstantBuffer();
	CreatePointLightSceneDataConstantBuffer();
	CreateShadowSceneDataConstantBuffer();
	CreatePointShadowSceneDataConstantBuffers();
	CreateGroundPlaneSceneDataConstantBuffer();
	CreateLightingDataConstantBuffer();
	CreateShadowPipelineState();
	CreateDoubleSidedShadowPipelineState();
	CreateGridVertexBuffer();
	CreatePointLightGizmoVertexBuffer();
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
		ShowDirectionalLightConfigWindow();
		return true;
	case IDM_VIEW_SOLID_GROUND_PLANE:
		m_useSolidGroundPlane = !m_useSolidGroundPlane;
		UpdateLightingMenuState();
		return true;
	case IDM_VIEW_ADD_POINT_LIGHT:
		SetPointLightEnabled(!m_pointLightEnabled);
		return true;
	default:
		return false;
	}
}

void ZenithRenderEngine::OnUpdate(const Timer& timer)
{
	UNREFERENCED_PARAMETER(timer);
	m_camera.Update();

	const XMMATRIX translate = XMMatrixTranslation(
		m_modelOffset.x,
		m_modelOffset.y,
		m_modelOffset.z);
	XMMATRIX world = translate;

	const XMMATRIX view = m_camera.GetViewMatrix();
	const XMMATRIX proj = m_camera.GetProjectionMatrix();
	const XMMATRIX normalMat = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	XMFLOAT3 directionalLightDirection = m_directionalLightDirection;
	if (XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&directionalLightDirection))) < 1e-6f)
	{
		directionalLightDirection = XMFLOAT3(-0.2f, -1.0f, -0.3f);
	}

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

	XMStoreFloat4x4(&m_sceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_sceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_sceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_sceneDataCbData.normalMatrix, normalMat);
	const float pointLightSceneScale = (std::max)(1.0f, shadowBoundsRadius);
	m_pointLightGizmoScale = (std::min)(
		PointLightGizmoScaleMax,
		(std::max)(PointLightGizmoScaleMin, pointLightSceneScale * PointLightGizmoSceneScaleFactor));
	const XMMATRIX pointLightWorld =
		XMMatrixScaling(m_pointLightGizmoScale, m_pointLightGizmoScale, m_pointLightGizmoScale) *
		XMMatrixTranslation(m_pointLightPosition.x, m_pointLightPosition.y, m_pointLightPosition.z);
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.model, XMMatrixTranspose(pointLightWorld));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_pointLightSceneDataCbData.normalMatrix, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.model, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.view, XMMatrixTranspose(view));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.projection, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&m_groundPlaneSceneDataCbData.normalMatrix, XMMatrixTranspose(XMMatrixIdentity()));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.model, XMMatrixTranspose(world));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.view, XMMatrixTranspose(lightView));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.projection, XMMatrixTranspose(lightProjection));
	XMStoreFloat4x4(&m_shadowSceneDataCbData.normalMatrix, normalMat);

	const XMVECTOR pointLightPositionVector = XMLoadFloat3(&m_pointLightPosition);
	const float pointShadowFarPlane = (std::max)(m_pointLightRange, PointShadowNearPlane + 0.1f);
	const XMMATRIX pointShadowProjection = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, PointShadowNearPlane, pointShadowFarPlane);
	const XMVECTOR pointShadowDirections[PointShadowFaceCount] = {
		XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet(-1.0f, 0.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f)
	};
	const XMVECTOR pointShadowUps[PointShadowFaceCount] = {
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f),
		XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
		XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
	};

	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		const XMMATRIX pointShadowView = XMMatrixLookAtLH(
			pointLightPositionVector,
			pointLightPositionVector + pointShadowDirections[faceIndex],
			pointShadowUps[faceIndex]);
		const XMMATRIX pointShadowViewProjection = pointShadowView * pointShadowProjection;

		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].model, XMMatrixTranspose(world));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].view, XMMatrixTranspose(pointShadowView));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].projection, XMMatrixTranspose(pointShadowProjection));
		XMStoreFloat4x4(&m_pointShadowSceneDataCbData[faceIndex].normalMatrix, normalMat);
		XMStoreFloat4x4(&m_lightingDataCbData.pointLightShadowViewProjection[faceIndex], XMMatrixTranspose(pointShadowViewProjection));
	}

	XMStoreFloat4(&m_lightingDataCbData.viewPosition, m_camera.GetPosition());

	const float directionalLightIntensity = m_directionalLightStrength * powf(2.0f, m_directionalLightExposure);
	m_lightingDataCbData.directionalLight.direction = XMFLOAT4(directionalLightDirection.x, directionalLightDirection.y, directionalLightDirection.z, 1.0f);
	if (m_directionalLightEnabled)
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.2f * directionalLightIntensity, 0.2f * directionalLightIntensity, 0.2f * directionalLightIntensity, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.5f * directionalLightIntensity, 0.5f * directionalLightIntensity, 0.5f * directionalLightIntensity, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(1.0f * directionalLightIntensity, 1.0f * directionalLightIntensity, 1.0f * directionalLightIntensity, 1.0f);
	}
	else
	{
		m_lightingDataCbData.directionalLight.ambient = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.diffuse = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		m_lightingDataCbData.directionalLight.specular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	XMStoreFloat4x4(&m_lightingDataCbData.lightViewProjection, XMMatrixTranspose(lightViewProjection));
	m_lightingDataCbData.shadowParams = XMFLOAT4(
		static_cast<float>(m_renderContext->GetShadowMapSrvIndex()),
		ShadowComparisonBias,
		FlipNormalMapGreenChannel ? -1.0f : 1.0f,
		m_directionalLightEnabled ? 1.0f : 0.0f);
	m_lightingDataCbData.pointLightPosition = XMFLOAT4(
		m_pointLightPosition.x,
		m_pointLightPosition.y,
		m_pointLightPosition.z,
		m_pointLightRange);
	m_lightingDataCbData.pointLightColor = XMFLOAT4(
		m_pointLightColor.x,
		m_pointLightColor.y,
		m_pointLightColor.z,
		m_pointLightEnabled ? (m_pointLightStrength * powf(2.0f, m_pointLightExposure)) : 0.0f);
	m_lightingDataCbData.pointShadowParams = XMFLOAT4(
		static_cast<float>(m_renderContext->GetPointShadowMapSrvIndex(0)),
		PointShadowComparisonBias,
		pointShadowFarPlane,
		m_pointLightEnabled ? 1.0f : 0.0f);

	memcpy(m_pSceneDataCbvDataBegin, &m_sceneDataCbData, sizeof(m_sceneDataCbData));
	memcpy(m_pPointLightSceneDataCbvDataBegin, &m_pointLightSceneDataCbData, sizeof(m_pointLightSceneDataCbData));
	memcpy(m_pGroundPlaneSceneDataCbvDataBegin, &m_groundPlaneSceneDataCbData, sizeof(m_groundPlaneSceneDataCbData));
	memcpy(m_pShadowSceneDataCbvDataBegin, &m_shadowSceneDataCbData, sizeof(m_shadowSceneDataCbData));
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		memcpy(
			m_pPointShadowSceneDataCbvDataBegin[faceIndex],
			&m_pointShadowSceneDataCbData[faceIndex],
			sizeof(m_pointShadowSceneDataCbData[faceIndex]));
	}
	memcpy(m_pLightingDataCbvDataBegin, &m_lightingDataCbData, sizeof(m_lightingDataCbData));
}

void ZenithRenderEngine::OnRender(const Timer& timer)
{
	UNREFERENCED_PARAMETER(timer);
	std::wstring capturePath = std::move(m_pendingRenderImagePath);
	m_pendingRenderImagePath.clear();

	m_renderContext->Prepare();

	auto commandList = m_renderContext->GetCommandList();
	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	auto device = m_renderContext->GetDevice();
	auto cbvSrvUavAllocator = m_renderContext->GetCbvSrvUavAllocator();
	const UINT shadowSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	UINT pointShadowSceneDescriptorIndices[PointShadowFaceCount] = {};
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		pointShadowSceneDescriptorIndices[faceIndex] = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	}
	const UINT pointLightSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT groundPlaneSceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT sceneDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();
	const UINT lightingDescriptorIndex = cbvSrvUavAllocator->AllocateDynamicDescriptor();

	D3D12_CONSTANT_BUFFER_VIEW_DESC shadowSceneCbvDesc = {};
	shadowSceneCbvDesc.BufferLocation = m_shadowSceneDataConstantBuffer->GetGPUVirtualAddress();
	shadowSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&shadowSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(shadowSceneDescriptorIndex));
	for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC pointShadowSceneCbvDesc = {};
		pointShadowSceneCbvDesc.BufferLocation = m_pointShadowSceneDataConstantBuffers[faceIndex]->GetGPUVirtualAddress();
		pointShadowSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
		device->CreateConstantBufferView(&pointShadowSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(pointShadowSceneDescriptorIndices[faceIndex]));
	}
	D3D12_CONSTANT_BUFFER_VIEW_DESC pointLightSceneCbvDesc = {};
	pointLightSceneCbvDesc.BufferLocation = m_pointLightSceneDataConstantBuffer->GetGPUVirtualAddress();
	pointLightSceneCbvDesc.SizeInBytes = (sizeof(SceneDataConstantBuffer) + 255) & ~255;
	device->CreateConstantBufferView(&pointLightSceneCbvDesc, cbvSrvUavAllocator->GetDynamicCpuHandle(pointLightSceneDescriptorIndex));
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

	ID3D12DescriptorHeap* descriptorHeaps[] = { cbvSrvUavAllocator->GetShaderVisibleHeap() };
	commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	commandList->SetGraphicsRootDescriptorTable(0, cbvSrvUavAllocator->GetDynamicGpuHandle(0));

	if (m_model)
	{
		auto shadowToDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE);
		commandList->ResourceBarrier(1, &shadowToDepthWrite);

		const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle = m_renderContext->GetShadowMapDsv();
		commandList->SetGraphicsRootSignature(m_rootSignature.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(shadowSceneDescriptorIndex));
		commandList->RSSetViewports(1, &m_renderContext->GetShadowMapViewport());
		commandList->RSSetScissorRects(1, &m_renderContext->GetShadowMapScissorRect());
		commandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);

		commandList->SetPipelineState(m_shadowPipelineState.Get());
		m_model->DrawOpaque(commandList, false);
		commandList->SetPipelineState(m_doubleSidedShadowPipelineState.Get());
		m_model->DrawOpaque(commandList, true);

		auto shadowToPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
			m_renderContext->GetShadowMap(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &shadowToPixelShaderResource);

		if (m_pointLightEnabled)
		{
			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				auto pointShadowToDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
					m_renderContext->GetPointShadowMap(faceIndex),
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					D3D12_RESOURCE_STATE_DEPTH_WRITE);
				commandList->ResourceBarrier(1, &pointShadowToDepthWrite);
			}

			commandList->SetGraphicsRootSignature(m_rootSignature.Get());
			commandList->RSSetViewports(1, &m_renderContext->GetPointShadowMapViewport());
			commandList->RSSetScissorRects(1, &m_renderContext->GetPointShadowMapScissorRect());

			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				const D3D12_CPU_DESCRIPTOR_HANDLE pointShadowDsvHandle = m_renderContext->GetPointShadowMapDsv(faceIndex);
				commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(pointShadowSceneDescriptorIndices[faceIndex]));
				commandList->ClearDepthStencilView(pointShadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
				commandList->OMSetRenderTargets(0, nullptr, FALSE, &pointShadowDsvHandle);

				commandList->SetPipelineState(m_shadowPipelineState.Get());
				m_model->DrawOpaque(commandList, false);
				commandList->SetPipelineState(m_doubleSidedShadowPipelineState.Get());
				m_model->DrawOpaque(commandList, true);
			}

			for (UINT faceIndex = 0; faceIndex < PointShadowFaceCount; ++faceIndex)
			{
				auto pointShadowToPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
					m_renderContext->GetPointShadowMap(faceIndex),
					D3D12_RESOURCE_STATE_DEPTH_WRITE,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				commandList->ResourceBarrier(1, &pointShadowToPixelShaderResource);
			}
		}
	}

	commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	commandList->SetGraphicsRootDescriptorTable(3, cbvSrvUavAllocator->GetDynamicGpuHandle(lightingDescriptorIndex));
	commandList->RSSetViewports(1, &m_viewport);
	commandList->RSSetScissorRects(1, &m_scissorRect);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
		m_renderContext->GetRtvHeapStart(),
		m_renderContext->GetCurrentFrameIndex(),
		m_renderContext->GetRtvDescriptorSize());
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_renderContext->GetDsvHeapStart());

	const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
	commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	if (m_useSolidGroundPlane)
	{
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
		commandList->SetPipelineState(m_gridPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(groundPlaneSceneDescriptorIndex));
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		commandList->IASetVertexBuffers(0, 1, &m_gridVertexBufferView);
		commandList->DrawInstanced(m_gridVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}

	commandList->SetPipelineState(m_pipelineState.Get());

	if (m_model)
	{
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

	if (m_pointLightEnabled && capturePath.empty())
	{
		commandList->SetPipelineState(m_pointLightGizmoPipelineState.Get());
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(pointLightSceneDescriptorIndex));
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		commandList->IASetVertexBuffers(0, 1, &m_pointLightGizmoVertexBufferView);
		commandList->DrawInstanced(m_pointLightGizmoVertexCount, 1, 0, 0);
		commandList->SetGraphicsRootDescriptorTable(1, cbvSrvUavAllocator->GetDynamicGpuHandle(sceneDescriptorIndex));
	}

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
	m_renderContext->WaitForGpu();
	DestroyDirectionalLightConfigWindow();
	DestroyPointLightConfigWindow();
}
