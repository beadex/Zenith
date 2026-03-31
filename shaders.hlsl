// ============================================================
// Shader overview for the current beginner shadow implementation
// ============================================================
//
// This file now contains three related rendering paths:
//   1. normal camera rendering (`VSMain` + `PSMain`)
//   2. grid rendering (`GridVSMain` + `GridPSMain`)
//   3. shadow-map rendering (`ShadowVSMain` + `ShadowPSMain`)
//
// The core shadow workflow is:
//   - `ShadowVSMain` / `ShadowPSMain` write depth from the light's view
//   - `PSMain` transforms each visible pixel into light space
//   - `ComputeShadowVisibility()` samples the shadow map with small PCF filtering
//   - ambient stays visible, while diffuse/specular are reduced by shadowing
//
// Features implemented so far in the shader:
//   - one directional light
//   - one shadow map
//   - 3x3 PCF soft shadow edges
//   - alpha-mask shadow casting for cutout materials
//   - separate diffuse/specular shadow response so highlights fade faster
// ============================================================

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
    uint normalStartIndex;
    uint numDiffuse;
    uint numSpecular;
    uint numOpacity;
    uint numNormal;
    // Matches the C++ MaterialAlphaMode* constants.
    uint alphaMode;
    // Used only for glTF alphaMode = MASK.
    float alphaCutoff;
    float2 padding;
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
    // z = normal-map green channel sign (+1 or -1)
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
    // Tangent-space basis imported from the mesh. These are not used directly as
    // lighting normals; instead they describe how to rotate a sampled normal-map
    // vector out of tangent space and into world space.
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 worldPos : POSITION;
    float2 uv : TEXCOORD;
    float3 tangent : TANGENT;
    float3 bitangent : BINORMAL;
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
// If a normal map exists, this helper reconstructs the final lighting normal by
// sampling the tangent-space texture and transforming it through the TBN basis.
float3 SampleNormal(float2 uv, float3 normal, float3 tangent, float3 bitangent);

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
    // The same normal matrix is used for the whole tangent basis so the pixel
    // shader receives all three vectors in a consistent space.
    vout.normal = normalize(mul(input.normal, (float3x3) sceneData.normalMatrix));
    vout.tangent = normalize(mul(input.tangent, (float3x3) sceneData.normalMatrix));
    vout.bitangent = normalize(mul(input.bitangent, (float3x3) sceneData.normalMatrix));
    vout.position = mul(mul(worldPos, sceneData.view), sceneData.projection);
    vout.uv = input.uv;

    return vout;
}

// ============================================================
// Pixel Shader
// ============================================================

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 norm = SampleNormal(input.uv, input.normal, input.tangent, input.bitangent);
    float3 viewDir = normalize(lightingData.viewPos.xyz - input.worldPos);
    float4 diffuseSample = SampleDiffuse(input.uv);

    // Ambient is intentionally kept separate from shadowing.
    // This means surfaces in shadow are not completely black; they still receive
    // a small amount of base/environment-style light.
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

float3 SampleNormal(float2 uv, float3 normal, float3 tangent, float3 bitangent)
{
    float3 norm = normalize(normal);

    // Materials without a normal map simply use the interpolated mesh normal,
    // which keeps old assets behaving exactly as they did before this feature.
    if (materialData.numNormal == 0)
    {
        return norm;
    }

    // Re-orthogonalize the tangent basis in the shader. Imported tangent data is
    // often close to correct but not perfectly orthogonal after interpolation.
    // Cleaning it up here gives a more stable TBN basis per pixel.
    float3 t = tangent - norm * dot(tangent, norm);
    float tangentLengthSq = dot(t, t);
    if (tangentLengthSq < 1e-6f)
    {
        return norm;
    }

    t *= rsqrt(tangentLengthSq);

    float3 b = bitangent - norm * dot(bitangent, norm);
    float bitangentLengthSq = dot(b, b);
    if (bitangentLengthSq < 1e-6f)
    {
        b = normalize(cross(norm, t));
    }
    else
    {
        b *= rsqrt(bitangentLengthSq);
    }

    // Normal maps are stored in texture space as 0..1 values, so they must be
    // unpacked back into the signed -1..1 vector range before lighting.
    float3 normalSample = gTextures[materialData.normalStartIndex].Sample(g_sampler, uv).xyz * 2.0f - 1.0f;
    // Some tools export tangent space with the opposite green-channel convention.
    // The renderer passes +1 or -1 through shadowParams.z so this one line can
    // support either convention without changing the asset.
    normalSample.y *= lightingData.shadowParams.z;
    float3x3 tbn = float3x3(t, b, norm);
    return normalize(mul(normalSample, tbn));
}

DirectLightResult CalcDirLight(DirectionalLightData light, float3 normal, float3 viewDir, float2 uv)
{
    DirectLightResult result;

    // This helper intentionally computes only DIRECT lighting terms.
    // Ambient is handled separately in `PSMain()` so shadowing can affect only:
    //   - diffuse
    //   - specular
    // while ambient remains visible.
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

    uint shadowMapIndex = (uint) lightingData.shadowParams.x;
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
