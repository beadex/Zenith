#pragma once

#include "D3D12Application.h"
#include "Model.h"
#include "Camera.h"

class ZenithRenderEngine : public D3D12Application
{
public:
	ZenithRenderEngine(UINT width, UINT height, std::wstring name);

	// Mandatory overrides for the application lifecycle. These will be called by the framework at the appropriate times.
	virtual void OnInit() override;
	virtual void OnUpdate(const Timer& timer) override;
	virtual void OnRender(const Timer& timer) override;
	virtual void OnDestroy() override;
	virtual bool OnCommand(UINT commandId) override;

	// Mouse input (routed from Win32Application)
	virtual void OnMouseMove(int x, int y, WPARAM btnState)  override;
	virtual void OnMiddleButtonDown(int x, int y)            override;
	virtual void OnMiddleButtonUp(int x, int y)              override;
	virtual void OnMouseWheel(float wheelDelta)              override;

private:
	// Here we will load the pipeline state, root signature, and any assets (like shaders, textures, etc.)

	// Pipeline objects
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;

	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
	ComPtr<ID3D12PipelineState> m_gridPipelineState;

	std::unique_ptr<Model> m_model;

	struct SceneDataConstantBuffer
	{
		XMFLOAT4X4 model;
		XMFLOAT4X4 view;
		XMFLOAT4X4 projection;
		XMFLOAT4X4 normalMatrix;
	};
	static_assert((sizeof(SceneDataConstantBuffer) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

	ComPtr<ID3D12Resource> m_sceneDataConstantBuffer;
	SceneDataConstantBuffer m_sceneDataCbData;
	UINT8* m_pSceneDataCbvDataBegin;
	std::wstring m_pendingRenderImagePath;

	struct GridVertex
	{
		XMFLOAT3 Position;
		XMFLOAT3 Color;
	};

	ComPtr<ID3D12Resource> m_gridVertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_gridVertexBufferView{};
	UINT m_gridVertexCount = 0;

	// Camera
	Camera m_camera;

	void CreateRootSignature();
	void CreatePipelineState();
	void CreateGridPipelineState();
	void CreateSceneDataConstantBuffer();
	void CreateGridVertexBuffer(const XMFLOAT3& center = XMFLOAT3(0.0f, 0.0f, 0.0f), float radius = 10.0f, float gridY = 0.0f);
	void LoadModelFromPath(const std::wstring& path);
};