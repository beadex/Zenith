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

    struct SceneConstantBuffer {
        XMMATRIX mvp;
    };

    ComPtr<ID3D12Resource> m_constantBuffer;
    SceneConstantBuffer m_cbData;
    UINT8* m_pCbvDataBegin;

    void CreateRootSignature();
    void CreatePipelineState();
    void CreateConstantBuffer();
};