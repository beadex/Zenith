#pragma once

#include "D3D12Application.h"
#include "Model.h"

class ZenithRenderEngine : public D3D12Application
{
public:
    ZenithRenderEngine(UINT width, UINT height, std::wstring name);

    // Mandatory overrides for the application lifecycle. These will be called by the framework at the appropriate times.
    virtual void OnInit() override;
    virtual void OnUpdate(const Timer& timer) override;
    virtual void OnRender(const Timer& timer) override;
    virtual void OnDestroy() override;

private:
    // Here we will load the pipeline state, root signature, and any assets (like shaders, textures, etc.)

    // Pipeline objects
    CD3DX12_VIEWPORT m_viewport;
    CD3DX12_RECT m_scissorRect;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;

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

    // Camera props
    XMVECTOR m_cameraPos;
    XMVECTOR m_cameraFront;
    XMVECTOR m_cameraUp;

    void CreateRootSignature();
    void CreatePipelineState();
    void CreateSceneDataConstantBuffer();
};