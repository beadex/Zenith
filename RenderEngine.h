#pragma once

#include "D3D12Application.h"

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

    void LoadPipeline();
    void LoadAssets();
};