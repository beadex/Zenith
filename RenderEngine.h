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
	virtual void OnLeftButtonDown(int x, int y)              override;
	virtual void OnLeftButtonUp(int x, int y)                override;
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
	static constexpr float PointShadowComparisonBias = 0.0012f;
	static constexpr float ShadowMinFrustumPadding = 1.0f;
	static constexpr float ShadowDepthRangePaddingScale = 0.5f;
	static constexpr UINT PointShadowFaceCount = 6;
	static constexpr float PointShadowNearPlane = 0.1f;
	// Different tools disagree about whether the normal-map green channel points
	// "up" or "down" in tangent space. Flipping this lets the renderer support
	// the opposite convention without re-exporting the asset.
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
	ComPtr<ID3D12PipelineState> m_pointLightGizmoPipelineState;
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
		// This is the inverse-transpose used to transform direction vectors such as
		// normals, tangents, and bitangents. Using the plain model matrix here would
		// be wrong once non-uniform scale is introduced.
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
	ComPtr<ID3D12Resource> m_pointShadowSceneDataConstantBuffers[PointShadowFaceCount];
	SceneDataConstantBuffer m_pointShadowSceneDataCbData[PointShadowFaceCount] = {};
	UINT8* m_pPointShadowSceneDataCbvDataBegin[PointShadowFaceCount] = {};
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
	ComPtr<ID3D12Resource> m_pointLightGizmoVertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_pointLightGizmoVertexBufferView{};
	UINT m_pointLightGizmoVertexCount = 0;
	ComPtr<ID3D12Resource> m_pointLightSceneDataConstantBuffer;
	SceneDataConstantBuffer m_pointLightSceneDataCbData;
	UINT8* m_pPointLightSceneDataCbvDataBegin = nullptr;
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
		XMFLOAT4 pointLightPosition;
		XMFLOAT4 pointLightColor;
		XMFLOAT4 pointShadowParams;
		XMFLOAT4X4 pointLightShadowViewProjection[PointShadowFaceCount];
		float padding[44];
	};
	static_assert((sizeof(LightingDataConstantBuffer) % 256) == 0, "Constant Buffer size must be 256-byte aligned");

	ComPtr<ID3D12Resource> m_lightingDataConstantBuffer;
	LightingDataConstantBuffer m_lightingDataCbData;
	UINT8* m_pLightingDataCbvDataBegin;
	// Directional light runtime state.
	  //
	  // Earlier versions of the sample hard-coded the sun direction and intensity.
	  // These fields make that lighting editable at runtime through the small tool
	  // window, which is especially helpful when learning how shadow direction,
	  // brightness, and exposure affect the final image.
	bool m_directionalLightEnabled = true;
	XMFLOAT3 m_directionalLightDirection = XMFLOAT3(-0.2f, -1.0f, -0.3f);
	float m_directionalLightStrength = 1.0f;
	float m_directionalLightExposure = 0.0f;
	// Win32 controls that back the directional-light configuration dialog.
	  // The dialog intentionally mixes exact numeric edit boxes with sliders:
	  //   - sliders are fast for experimentation
	  //   - edit boxes still allow precise values
	HWND m_directionalLightConfigWindow = nullptr;
	HWND m_directionalLightEnabledCheck = nullptr;
	HWND m_directionalLightDirectionXEdit = nullptr;
	HWND m_directionalLightDirectionYEdit = nullptr;
	HWND m_directionalLightDirectionZEdit = nullptr;
	HWND m_directionalLightDirectionXSlider = nullptr;
	HWND m_directionalLightDirectionYSlider = nullptr;
	HWND m_directionalLightDirectionZSlider = nullptr;
	HWND m_directionalLightStrengthEdit = nullptr;
	HWND m_directionalLightExposureEdit = nullptr;
	HWND m_directionalLightStrengthSlider = nullptr;
	HWND m_directionalLightExposureSlider = nullptr;
	static constexpr float PointLightDragPlaneHeight = 2.0f;
	static constexpr float PointLightGizmoScaleMin = 0.75f;
	static constexpr float PointLightGizmoScaleMax = 25.0f;
	static constexpr float PointLightGizmoSceneScaleFactor = 0.08f;
	static constexpr float PointLightGizmoHitPaddingPixels = 8.0f;
	static constexpr float PointLightVerticalDragSensitivity = 0.0025f;
	// Point light runtime state.
	 //
	 // The point light can be added/removed from the menu, dragged in the viewport,
	 // and edited through its own dialog. Strength/exposure/range live here because
	 // they feed both lighting and point-shadow setup every frame.
	bool m_pointLightEnabled = false;
	XMFLOAT3 m_pointLightPosition = XMFLOAT3(0.0f, PointLightDragPlaneHeight, 0.0f);
	XMFLOAT3 m_pointLightColor = XMFLOAT3(1.0f, 0.95f, 0.80f);
	float m_pointLightRange = 20.0f;
	float m_pointLightStrength = 64.0f;
	float m_pointLightExposure = 0.0f;
	float m_pointLightGizmoScale = PointLightGizmoScaleMin;
	XMFLOAT3 m_pointLightDragPlanePoint = XMFLOAT3(0.0f, PointLightDragPlaneHeight, 0.0f);
	XMFLOAT3 m_pointLightDragPlaneNormal = XMFLOAT3(0.0f, 0.0f, 1.0f);
	XMFLOAT3 m_pointLightDragOffset = XMFLOAT3(0.0f, 0.0f, 0.0f);
	bool m_isPointLightHovered = false;
	bool m_isPointLightDragging = false;
	POINT m_lastLeftMousePosition{};
	// Win32 controls used by the point-light tool window.
	// This mirrors the directional-light dialog structure so both lights can be
	// adjusted in a consistent way.
	HWND m_pointLightConfigWindow = nullptr;
	HWND m_pointLightStrengthEdit = nullptr;
	HWND m_pointLightExposureEdit = nullptr;
	HWND m_pointLightRangeEdit = nullptr;
	HWND m_pointLightStrengthSlider = nullptr;
	HWND m_pointLightExposureSlider = nullptr;
	HWND m_pointLightRangeSlider = nullptr;
	HWND m_pointLightColorText = nullptr;

	// Camera
	Camera m_camera;

	void CreateRootSignature();
	void CreatePipelineState();
	void CreateDoubleSidedPipelineState();
	void CreateTransparentPipelineState();
	void CreateDoubleSidedTransparentPipelineState();
	void CreateGridPipelineState();
	void CreatePointLightGizmoPipelineState();
	void CreateShadowPipelineState();
	void CreateDoubleSidedShadowPipelineState();
	void CreateSceneDataConstantBuffer();
	void CreateShadowSceneDataConstantBuffer();
	void CreatePointShadowSceneDataConstantBuffers();
	void CreateLightingDataConstantBuffer();
	void CreateGridVertexBuffer(float radius = 10.0f);
	// Directional-light tool window helpers.
	void EnsureDirectionalLightConfigWindow();
	void ShowDirectionalLightConfigWindow();
	void SyncDirectionalLightConfigWindow() const;
	void ApplyDirectionalLightConfigFromWindow();
	void DestroyDirectionalLightConfigWindow();
	void SetDirectionalLightEnabled(bool enabled);
	static LRESULT CALLBACK DirectionalLightConfigWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	// Point-light tool window + viewport gizmo helpers.
	void CreatePointLightGizmoVertexBuffer();
	void UpdatePointLightGizmoVertexBuffer();
	void CreatePointLightSceneDataConstantBuffer();
	void EnsurePointLightConfigWindow();
	void ShowPointLightConfigWindow();
	void SyncPointLightConfigWindow() const;
	void ApplyPointLightConfigFromWindow();
	void ChoosePointLightColor();
	void DestroyPointLightConfigWindow();
	void RenderPointLightPreviewFrame();
	void SetPointLightEnabled(bool enabled);
	void UpdatePointLightHoverState(int x, int y);
	static LRESULT CALLBACK PointLightConfigWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	void CreateGroundPlaneVertexBuffer(float radius = 10.0f);
	void CreateGroundPlaneSceneDataConstantBuffer();
	void CreateGroundPlaneMaterialConstantBuffer();
	void LoadModelFromPath(const std::wstring& path);
	void UpdateLightingMenuState() const;
};