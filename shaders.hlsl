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
//   - one point light with inverse-square-style attenuation
//   - six point-shadow faces used like a cubemap shadow setup
//   - 3x3 PCF soft shadow edges
//   - wider weighted PCF for point shadows
//   - alpha-mask shadow casting for cutout materials
//   - seam blending between neighboring point-shadow faces
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
    float4 pointLightPosition;
    float4 pointLightColor;
    // x = first point-shadow map descriptor index in gTextures[]
    // y = point-shadow comparison bias
    // z = point-shadow far plane
    // w = 1 when point-light shadowing is enabled
    float4 pointShadowParams;
    float4x4 pointLightShadowViewProjection[6];
};
ConstantBuffer<LightingData> lightingData : register(b2);

// ============================================================
// Resources
// ============================================================

Texture2D gTextures[] : register(t0, space0);
SamplerState g_sampler : register(s0);

// Point-shadow tuning values.
//
// These are intentionally grouped together because they shape the look of the
// point-light shadow system in four different ways:
//   1. PCF kernel size
//   2. how soft the filter becomes with distance
//   3. how much extra softness is added at grazing angles
//   4. how aggressively neighboring cubemap faces are blended near seams
static const int POINT_SHADOW_PCF_KERNEL_RADIUS = 2;
static const float POINT_SHADOW_PCF_MIN_RADIUS_TEXELS = 0.85f;
static const float POINT_SHADOW_PCF_MAX_RADIUS_TEXELS = 1.85f;
static const float POINT_SHADOW_PCF_GRAZING_BOOST_TEXELS = 0.30f;
static const float POINT_SHADOW_FACE_BLEND_GAP_MIN = 0.02f;
static const float POINT_SHADOW_FACE_BLEND_GAP_MAX = 0.18f;
// Small floor value used by inverse-square attenuation to avoid dividing by a
// value that approaches zero when the shaded point is extremely close to light.
static const float POINT_LIGHT_INVERSE_SQUARE_EPSILON = 0.01f;

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
DirectLightResult CalcPointLight(float3 worldPos, float3 normal, float3 viewDir, float2 uv);
float ComputeShadowVisibility(float3 worldPos);
// Point-shadow evaluation is split across helpers so each step is easier to
// understand:
//   - choose the dominant cubemap-like face
//   - optionally blend with the second-best face near seams
//   - project into that face and compute filtered visibility there
float ComputePointShadowVisibility(float3 worldPos, float3 normal);
float ComputePointShadowVisibilityForFace(uint faceIndex, float3 worldPos, float3 normal, float distanceToLight, float normalAlignment);
uint GetPointShadowFaceIndex(uint axisIndex, bool positiveDirection);
float ComputeWeightedPointShadowVisibility(uint pointShadowMapIndex, float2 shadowUv, float currentDepth, uint pointShadowMapWidth, uint pointShadowMapHeight, float filterRadiusTexels);

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
    DirectLightResult pointLight = CalcPointLight(input.worldPos, norm, viewDir, input.uv);
    float shadow = ComputeShadowVisibility(input.worldPos);
    float pointShadow = ComputePointShadowVisibility(input.worldPos, norm);

    // Shadow visibility comes from PCF, so it is already a soft 0..1 value.
    // We use it directly for diffuse, but square it for specular so highlights
    // disappear sooner inside penumbra and full shadow.
    float specularShadow = shadow * shadow;
    float pointSpecularShadow = pointShadow * pointShadow;
    float3 result = ambient + directLight.diffuse * shadow + directLight.specular * specularShadow + pointLight.diffuse * pointShadow + pointLight.specular * pointSpecularShadow;

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

DirectLightResult CalcPointLight(float3 worldPos, float3 normal, float3 viewDir, float2 uv)
{
    DirectLightResult result;
    result.diffuse = 0.0f;
    result.specular = 0.0f;

    float range = lightingData.pointLightPosition.w;
    float intensity = lightingData.pointLightColor.w;
    if (range <= 0.0f || intensity <= 0.0f)
    {
        return result;
    }

    float3 lightVector = lightingData.pointLightPosition.xyz - worldPos;
    float distanceToLight = length(lightVector);
    if (distanceToLight >= range || distanceToLight <= 1e-5f)
    {
        return result;
    }

    float3 albedo = SampleDiffuse(uv).rgb;
    float3 specMap = float3(1.0f, 1.0f, 1.0f);
    if (materialData.numSpecular > 0)
    {
        specMap = gTextures[materialData.specularStartIndex].Sample(g_sampler, uv).rgb;
    }

    float3 lightDir = lightVector / distanceToLight;
    // The point light now uses a more natural falloff than the older
    // `saturate(1 - d/r)^2` style. The lighting is based on two parts:
    //   1. inverse-square attenuation          -> physically motivated falloff
    //   2. smooth range fade                   -> artist-friendly finite cutoff
    //
    // Without the range fade, the light would technically influence everything
    // forever. Without the inverse-square term, the falloff would feel less
    // natural when moving the light near or far from the model.
    float distanceRatio = saturate(distanceToLight / range);
    float rangeFade = saturate(1.0f - distanceRatio * distanceRatio * distanceRatio * distanceRatio);
    rangeFade *= rangeFade;
    float attenuation = rangeFade / max(distanceToLight * distanceToLight, POINT_LIGHT_INVERSE_SQUARE_EPSILON);

    float diff = max(dot(normal, lightDir), 0.0f);
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0f), 0.6f * 128.0f);
    float3 lightColor = lightingData.pointLightColor.rgb * intensity * attenuation;

    result.diffuse = lightColor * diff * albedo;
    result.specular = lightColor * spec * specMap;
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

float ComputePointShadowVisibility(float3 worldPos, float3 normal)
{
    if (lightingData.pointShadowParams.w < 0.5f)
    {
        return 1.0f;
    }

    float3 lightToSurface = worldPos - lightingData.pointLightPosition.xyz;
    float3 axisWeights = abs(lightToSurface);
    float distanceToLight = length(lightToSurface);
    float3 lightDir = normalize(lightingData.pointLightPosition.xyz - worldPos);
    float normalAlignment = saturate(dot(normal, lightDir));

    // A real cubemap shadow lookup would automatically pick the correct cube
    // face from the sample direction. This beginner version stores six regular
    // 2D shadow maps instead, so the shader manually chooses which face best
    // matches the light-to-surface direction.

    uint primaryAxis = 0u;
    uint secondaryAxis = 1u;
    if (axisWeights.y > axisWeights.x)
    {
        primaryAxis = 1u;
        secondaryAxis = 0u;
    }
    if (axisWeights.z > axisWeights[primaryAxis])
    {
        secondaryAxis = primaryAxis;
        primaryAxis = 2u;
    }
    else if (axisWeights.z > axisWeights[secondaryAxis])
    {
        secondaryAxis = 2u;
    }

    uint faceIndex = GetPointShadowFaceIndex(primaryAxis, lightToSurface[primaryAxis] >= 0.0f);
    float primaryVisibility = ComputePointShadowVisibilityForFace(faceIndex, worldPos, normal, distanceToLight, normalAlignment);
    if (primaryVisibility < 0.0f)
    {
        return 1.0f;
    }

    float primaryWeight = axisWeights[primaryAxis];
    float secondaryWeight = axisWeights[secondaryAxis];
    // If two axes are similarly strong, the direction is near a face edge.
    // In that case the shader blends the result from the primary and secondary
    // face instead of hard-switching from one face to the other. This reduces
    // visible seams where the six point-shadow projections meet.
    float axisGap = (primaryWeight - secondaryWeight) / max(primaryWeight, 1e-5f);
    float seamBlendWeight = 1.0f - smoothstep(POINT_SHADOW_FACE_BLEND_GAP_MIN, POINT_SHADOW_FACE_BLEND_GAP_MAX, axisGap);
    if (seamBlendWeight <= 0.0f || secondaryWeight <= 1e-5f)
    {
        return primaryVisibility;
    }

    uint secondaryFaceIndex = GetPointShadowFaceIndex(secondaryAxis, lightToSurface[secondaryAxis] >= 0.0f);
    float secondaryVisibility = ComputePointShadowVisibilityForFace(secondaryFaceIndex, worldPos, normal, distanceToLight, normalAlignment);
    if (secondaryVisibility < 0.0f)
    {
        return primaryVisibility;
    }

    float weightedVisibility = (primaryVisibility * primaryWeight + secondaryVisibility * secondaryWeight) / max(primaryWeight + secondaryWeight, 1e-5f);
    return lerp(primaryVisibility, weightedVisibility, seamBlendWeight);
}

float ComputePointShadowVisibilityForFace(uint faceIndex, float3 worldPos, float3 normal, float distanceToLight, float normalAlignment)
{
    // Project the current world-space point through one of the six point-light
    // shadow cameras. If the projected point lies outside that face, the helper
    // returns -1 so the caller knows this face is not usable for that sample.
    float4 lightClipPos = mul(float4(worldPos, 1.0f), lightingData.pointLightShadowViewProjection[faceIndex]);
    if (lightClipPos.w <= 0.0f)
    {
        return -1.0f;
    }

    float3 shadowPos = lightClipPos.xyz / lightClipPos.w;
    float2 shadowUv = float2(shadowPos.x * 0.5f + 0.5f, shadowPos.y * -0.5f + 0.5f);

    if (shadowUv.x < 0.0f || shadowUv.x > 1.0f || shadowUv.y < 0.0f || shadowUv.y > 1.0f || shadowPos.z < 0.0f || shadowPos.z > 1.0f)
    {
        return -1.0f;
    }

    uint pointShadowMapIndex = (uint) lightingData.pointShadowParams.x + faceIndex;
    uint pointShadowMapWidth = 0;
    uint pointShadowMapHeight = 0;
    gTextures[pointShadowMapIndex].GetDimensions(pointShadowMapWidth, pointShadowMapHeight);

    // Point-shadow softness is not fixed. The filter grows with distance so
    // farther surfaces get a slightly softer penumbra, then gets a small extra
    // boost at grazing angles where shadow acne and aliasing are usually more
    // noticeable.
    float distanceRatio = saturate(distanceToLight / max(lightingData.pointLightPosition.w, 1e-5f));
    float softenedDistanceRatio = distanceRatio * distanceRatio * (3.0f - 2.0f * distanceRatio);
    float baseFilterRadiusTexels = lerp(
        POINT_SHADOW_PCF_MIN_RADIUS_TEXELS,
        POINT_SHADOW_PCF_MAX_RADIUS_TEXELS,
        softenedDistanceRatio);
    baseFilterRadiusTexels += (1.0f - normalAlignment) * POINT_SHADOW_PCF_GRAZING_BOOST_TEXELS;
    float edgeDistanceTexels = min(
        min(shadowUv.x, 1.0f - shadowUv.x) * pointShadowMapWidth,
        min(shadowUv.y, 1.0f - shadowUv.y) * pointShadowMapHeight);
    // Near the border of a face, the filter radius is clamped down so a wide PCF
    // kernel does not pull too much information from the very edge of the map.
    float filterRadiusTexels = min(baseFilterRadiusTexels, max(0.0f, edgeDistanceTexels - 1.0f));

    // The point-shadow comparison bias is angle-aware. Surfaces that face away
    // from the light get a larger bias, which helps suppress acne, while surfaces
    // facing the light keep a smaller bias so contact shadows stay tighter.
    float bias = max(lightingData.pointShadowParams.y * (1.0f - normalAlignment), lightingData.pointShadowParams.y * 0.35f);
    float currentDepth = shadowPos.z - bias;

    return ComputeWeightedPointShadowVisibility(
        pointShadowMapIndex,
        shadowUv,
        currentDepth,
        pointShadowMapWidth,
        pointShadowMapHeight,
        filterRadiusTexels);
}

uint GetPointShadowFaceIndex(uint axisIndex, bool positiveDirection)
{
    if (axisIndex == 0u)
    {
        return positiveDirection ? 0u : 1u;
    }

    if (axisIndex == 1u)
    {
        return positiveDirection ? 2u : 3u;
    }

    return positiveDirection ? 4u : 5u;
}

float ComputeWeightedPointShadowVisibility(uint pointShadowMapIndex, float2 shadowUv, float currentDepth, uint pointShadowMapWidth, uint pointShadowMapHeight, float filterRadiusTexels)
{
    if (filterRadiusTexels <= 0.5f)
    {
        // If the requested radius is tiny, there is no reason to run the whole
        // PCF kernel. A single comparison is cheaper and behaves almost the same.
        float shadowDepth = gTextures[pointShadowMapIndex].Sample(g_sampler, shadowUv).r;
        return (currentDepth <= shadowDepth) ? 1.0f : 0.0f;
    }

    float2 texelSize = 1.0f / float2(pointShadowMapWidth, pointShadowMapHeight);
    float kernelScale = filterRadiusTexels / POINT_SHADOW_PCF_KERNEL_RADIUS;
    float litWeight = 0.0f;
    float totalWeight = 0.0f;

    // This is a weighted 5x5 PCF filter. Samples near the center receive more
    // weight than samples near the corners, which usually gives a smoother and
    // slightly more stable result than a flat average over the same footprint.

    [unroll]
    for (int y = -POINT_SHADOW_PCF_KERNEL_RADIUS; y <= POINT_SHADOW_PCF_KERNEL_RADIUS; ++y)
    {
        [unroll]
        for (int x = -POINT_SHADOW_PCF_KERNEL_RADIUS; x <= POINT_SHADOW_PCF_KERNEL_RADIUS; ++x)
        {
            float2 sampleOffset = float2(x, y) * texelSize * kernelScale;
            float2 sampleUv = saturate(shadowUv + sampleOffset);
            float shadowDepth = gTextures[pointShadowMapIndex].Sample(g_sampler, sampleUv).r;
            float weight = float((POINT_SHADOW_PCF_KERNEL_RADIUS + 1 - abs(x)) * (POINT_SHADOW_PCF_KERNEL_RADIUS + 1 - abs(y)));
            litWeight += ((currentDepth <= shadowDepth) ? 1.0f : 0.0f) * weight;
            totalWeight += weight;
        }
    }

    return litWeight / max(totalWeight, 1e-5f);
}

GridPSInput GridVSMain(GridVSInput input)
{
    GridPSInput output;

    float4 worldPos = mul(float4(input.position, 1.0f), sceneData.model);
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
