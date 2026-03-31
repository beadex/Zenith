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
	// Beginner-friendly shadow tuning values.
	//
	// These are kept here so the first shadow-map implementation can be tuned in
	// one place instead of hunting through PSO creation and shader upload code.
	static constexpr INT ShadowRasterDepthBias = 1000;
	static constexpr float ShadowRasterSlopeScaledDepthBias = 1.0f;
	static constexpr float ShadowComparisonBias = 0.0015f;
	static constexpr float ShadowMinFrustumPadding = 1.0f;
	static constexpr float ShadowDepthRangePaddingScale = 0.5f;
	static constexpr bool FlipNormalMapGreenChannel = false;

	// The renderer now keeps four model PSOs instead of one so it can route each
	// mesh by two independent material properties:
	//   1. opaque vs. transparent
	//   2. single-sided vs. double-sided
	//
	// This avoids the old "one transparent PSO for everything" behavior, which
	// was simple but forced all transparent meshes to render with culling disabled.
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12PipelineState> m_pipelineState;
	ComPtr<ID3D12PipelineState> m_doubleSidedPipelineState;
	ComPtr<ID3D12PipelineState> m_transparentPipelineState;
	ComPtr<ID3D12PipelineState> m_doubleSidedTransparentPipelineState;
	ComPtr<ID3D12PipelineState> m_gridPipelineState;
	// These two PSOs are used only for the shadow pass.
	  // They render depth from the light's point of view, not color to the screen.
	ComPtr<ID3D12PipelineState> m_shadowPipelineState;
	ComPtr<ID3D12PipelineState> m_doubleSidedShadowPipelineState;

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
	// Separate SceneData for the shadow pass. It uses the same model transform but
	   // different view/projection matrices because the camera is the light.
	ComPtr<ID3D12Resource> m_shadowSceneDataConstantBuffer;
	SceneDataConstantBuffer m_shadowSceneDataCbData;
	UINT8* m_pShadowSceneDataCbvDataBegin;
	std::wstring m_pendingRenderImagePath;
	XMFLOAT3 m_modelOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);

	struct GridVertex
	{
		XMFLOAT3 Position;
		XMFLOAT3 Color;
	};

	ComPtr<ID3D12Resource> m_gridVertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_gridVertexBufferView{};
	UINT m_gridVertexCount = 0;
	// Optional solid plane used to make shadows easier to see than on a line grid.
	ComPtr<ID3D12Resource> m_groundPlaneVertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_groundPlaneVertexBufferView{};
	UINT m_groundPlaneVertexCount = 0;
	ComPtr<ID3D12Resource> m_groundPlaneSceneDataConstantBuffer;
	SceneDataConstantBuffer m_groundPlaneSceneDataCbData;
	UINT8* m_pGroundPlaneSceneDataCbvDataBegin = nullptr;
	ComPtr<ID3D12Resource> m_groundPlaneMaterialConstantBuffer;
	MaterialData m_groundPlaneMaterialData;
	UINT8* m_pGroundPlaneMaterialCbvDataBegin = nullptr;
	bool m_useSolidGroundPlane = false;

	struct DirectionalLightData
	{
		XMFLOAT4 direction;
		XMFLOAT4 ambient;
		XMFLOAT4 diffuse;
		XMFLOAT4 specular;
	};

	struct LightingDataConstantBuffer
	{
		DirectionalLightData directionalLight;
		XMFLOAT4 viewPosition;
		// Main-pass pixels are transformed into this light clip space so they can
		   // look themselves up in the shadow map.
		XMFLOAT4X4 lightViewProjection;
		// x = shadow map SRV index
		  // y = depth bias used during comparison
	   // z = normal-map green channel sign (+1 or -1)
		  // w = 1 when directional light/shadows are enabled
		XMFLOAT4 shadowParams;
		float padding[24];
	};
	static_assert((sizeof(LightingDataConstantBuffer) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

	ComPtr<ID3D12Resource> m_lightingDataConstantBuffer;
	LightingDataConstantBuffer m_lightingDataCbData;
	UINT8* m_pLightingDataCbvDataBegin;
	bool m_directionalLightEnabled = true;

	// Camera
	Camera m_camera;

	void CreateRootSignature();
	void CreatePipelineState();
	void CreateDoubleSidedPipelineState();
	void CreateTransparentPipelineState();
	void CreateDoubleSidedTransparentPipelineState();
	void CreateGridPipelineState();
	void CreateShadowPipelineState();
	void CreateDoubleSidedShadowPipelineState();
	void CreateSceneDataConstantBuffer();
	void CreateShadowSceneDataConstantBuffer();
	void CreateLightingDataConstantBuffer();
	void CreateGridVertexBuffer(float radius = 10.0f);
	void CreateGroundPlaneVertexBuffer(float radius = 10.0f);
	void CreateGroundPlaneSceneDataConstantBuffer();
	void CreateGroundPlaneMaterialConstantBuffer();
	void LoadModelFromPath(const std::wstring& path);
	void UpdateLightingMenuState() const;
};