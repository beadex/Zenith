// ============================================================
// Constant Buffers
// ============================================================

struct SceneData
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float4x4 normalMatrix;
};
ConstantBuffer<SceneData> sceneData : register(b0);

struct MaterialData
{
    uint diffuseStartIndex;
    uint specularStartIndex;
    uint opacityStartIndex;
    uint numDiffuse;
    uint numSpecular;
    uint numOpacity;
    // Matches the C++ MaterialAlphaMode* constants.
    uint alphaMode;
    // Used only for glTF alphaMode = MASK.
    float alphaCutoff;
    // Lightweight glTF support: material tint and alpha factor.
    float4 baseColorFactor;
};
ConstantBuffer<MaterialData> materialData : register(b1);

static const uint ALPHA_MODE_OPAQUE = 0u;
static const uint ALPHA_MODE_MASK = 1u;
static const uint ALPHA_MODE_BLEND = 2u;

struct DirectionalLightData
{
    float4 direction;
    float4 ambient;
    float4 diffuse;
    float4 specular;
};

struct LightingData
{
    DirectionalLightData directionalLight;
    float4 viewPos;
    // Used to convert a world-space point from the main pass into the light's clip space.
    float4x4 lightViewProjection;
    // x = shadow map descriptor index in gTextures[]
    // y = depth bias
    // z = reserved
    // w = 1 when shadowing is enabled
    float4 shadowParams;
};
ConstantBuffer<LightingData> lightingData : register(b2);

// ============================================================
// Resources
// ============================================================

Texture2D gTextures[] : register(t0, space0);
SamplerState g_sampler : register(s0);

// ============================================================
// Vertex / Pixel shader structs
// ============================================================

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 worldPos : POSITION;
    float2 uv : TEXCOORD;
};

struct ShadowPSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

struct GridVSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
};

struct GridPSInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

float4 SampleDiffuse(float2 uv);
float SampleOpacity(float2 uv);

// For the beginner shadow implementation, it helps to keep direct-light pieces
// separate so the shader can treat them differently in shadow:
//   - diffuse  -> fades with normal PCF visibility
//   - specular -> fades even faster so shadowed highlights do not linger
struct DirectLightResult
{
    float3 diffuse;
    float3 specular;
};

DirectLightResult CalcDirLight(DirectionalLightData light, float3 normal, float3 viewDir, float2 uv);
float ComputeShadowVisibility(float3 worldPos);

// ============================================================
// Vertex Shader
// ============================================================

PSInput VSMain(VSInput input)
{
    PSInput vout;

    float4 worldPos = mul(float4(input.position, 1.0f), sceneData.model);
    vout.worldPos = worldPos.xyz;
    vout.normal = normalize(mul(input.normal, (float3x3) sceneData.normalMatrix));
    vout.position = mul(mul(worldPos, sceneData.view), sceneData.projection);
    vout.uv = input.uv;

    return vout;
}

// ============================================================
// Pixel Shader
// ============================================================

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 norm = normalize(input.normal);
    float3 viewDir = normalize(lightingData.viewPos.xyz - input.worldPos);
    float4 diffuseSample = SampleDiffuse(input.uv);
    float3 ambient = lightingData.directionalLight.ambient.rgb * diffuseSample.rgb;
    float opacity = diffuseSample.a * SampleOpacity(input.uv);

  // The shader supports the three major glTF alpha modes used by the renderer:
    //   OPAQUE -> force alpha to 1
    //   MASK   -> alpha test with cutoff, then write opaque result
    //   BLEND  -> keep alpha for the transparent pass
    if (materialData.alphaMode == ALPHA_MODE_MASK)
    {
        clip(opacity - materialData.alphaCutoff);
        opacity = 1.0f;
    }
    else if (materialData.alphaMode == ALPHA_MODE_BLEND)
    {
        // Fully transparent texels should not update depth, otherwise hidden parts
        // of cutout textures can still occlude geometry behind them.
        clip(opacity - 0.01f);
    }
    else
    {
        opacity = 1.0f;
    }

    DirectLightResult directLight = CalcDirLight(lightingData.directionalLight, norm, viewDir, input.uv);
    float shadow = ComputeShadowVisibility(input.worldPos);

    // Shadow visibility comes from PCF, so it is already a soft 0..1 value.
    // We use it directly for diffuse, but square it for specular so highlights
    // disappear sooner inside penumbra and full shadow.
    float specularShadow = shadow * shadow;
    float3 result = ambient + directLight.diffuse * shadow + directLight.specular * specularShadow;

    const float gamma = 2.2f;
    float3 gammaCorrected = pow(saturate(result), 1.0f / gamma);

    return float4(gammaCorrected, opacity);
}

float4 SampleDiffuse(float2 uv)
{
    if (materialData.numDiffuse > 0)
    {
        return gTextures[materialData.diffuseStartIndex].Sample(g_sampler, uv) * materialData.baseColorFactor;
    }

    return materialData.baseColorFactor;
}

float SampleOpacity(float2 uv)
{
    if (materialData.numOpacity > 0)
    {
        return gTextures[materialData.opacityStartIndex].Sample(g_sampler, uv).r;
    }

    return 1.0f;
}

DirectLightResult CalcDirLight(DirectionalLightData light, float3 normal, float3 viewDir, float2 uv)
{
    DirectLightResult result;
    float3 albedo = SampleDiffuse(uv).rgb;
    float3 specMap = float3(1.0f, 1.0f, 1.0f);

    if (materialData.numSpecular > 0)
    {
        specMap = gTextures[materialData.specularStartIndex].Sample(g_sampler, uv).rgb;
    }

    float3 lightDir = normalize(-light.direction.xyz);
    float diff = max(dot(normal, lightDir), 0.0f);

    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0f), 0.6f * 128.0f);

    result.diffuse = light.diffuse.rgb * diff * albedo;
    result.specular = light.specular.rgb * spec * specMap;

    return result;
}

float ComputeShadowVisibility(float3 worldPos)
{
    if (lightingData.shadowParams.w < 0.5f)
    {
        return 1.0f;
    }

    // Reproject the current main-pass pixel into the light's clip space.
    float4 lightClipPos = mul(float4(worldPos, 1.0f), lightingData.lightViewProjection);
    if (lightClipPos.w <= 0.0f)
    {
        return 1.0f;
    }

    // Convert from clip/NDC space into texture UV space.
    float3 shadowPos = lightClipPos.xyz / lightClipPos.w;
    float2 shadowUv = float2(shadowPos.x * 0.5f + 0.5f, shadowPos.y * -0.5f + 0.5f);

    if (shadowUv.x < 0.0f || shadowUv.x > 1.0f || shadowUv.y < 0.0f || shadowUv.y > 1.0f || shadowPos.z < 0.0f || shadowPos.z > 1.0f)
    {
        return 1.0f;
    }

    uint shadowMapIndex = (uint)lightingData.shadowParams.x;
    uint shadowMapWidth = 0;
    uint shadowMapHeight = 0;
    gTextures[shadowMapIndex].GetDimensions(shadowMapWidth, shadowMapHeight);

    // PCF = Percentage Closer Filtering.
    // Instead of one hard compare, sample a small 3x3 neighborhood and average.
    // The returned value is a soft visibility factor in the 0..1 range:
    //   1.0 -> fully lit
    //   0.0 -> fully shadowed
    // values in-between -> penumbra / softened edge
    float2 texelSize = 1.0f / float2(shadowMapWidth, shadowMapHeight);
    float currentDepth = shadowPos.z - lightingData.shadowParams.y;
    float litSampleCount = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 sampleUv = saturate(shadowUv + float2(x, y) * texelSize);
            float shadowDepth = gTextures[shadowMapIndex].Sample(g_sampler, sampleUv).r;
            litSampleCount += (currentDepth <= shadowDepth) ? 1.0f : 0.0f;
        }
    }

    return litSampleCount / 9.0f;
}

GridPSInput GridVSMain(GridVSInput input)
{
    GridPSInput output;

    float4 worldPos = float4(input.position, 1.0f);
    output.position = mul(mul(worldPos, sceneData.view), sceneData.projection);
    output.color = input.color;

    return output;
}

float4 GridPSMain(GridPSInput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}

ShadowPSInput ShadowVSMain(VSInput input)
{
    // The shadow pass only needs vertex positions transformed by the light camera.
    // For alpha-mask materials, UVs are also forwarded so the shadow pixel shader
    // can discard transparent texels before depth is written.
    ShadowPSInput output;
    float4 worldPos = mul(float4(input.position, 1.0f), sceneData.model);
    output.position = mul(mul(worldPos, sceneData.view), sceneData.projection);
    output.uv = input.uv;
    return output;
}

void ShadowPSMain(ShadowPSInput input)
{
    // Mask materials should cast cutout shadows that match their visible shape.
    // Opaque materials simply fall through and write depth normally.
    if (materialData.alphaMode == ALPHA_MODE_MASK)
    {
        float opacity = SampleDiffuse(input.uv).a * SampleOpacity(input.uv);
        clip(opacity - materialData.alphaCutoff);
    }
}
