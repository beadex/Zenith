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
    uint numDiffuse;
    uint numSpecular;
};
ConstantBuffer<MaterialData> materialData : register(b1);

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

float3 CalcDirLight(DirectionalLightData light, float3 normal, float3 viewDir, float2 uv);

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

    float3 result = CalcDirLight(lightingData.directionalLight, norm, viewDir, input.uv);

    const float gamma = 2.2f;
    float3 gammaCorrected = pow(saturate(result), 1.0f / gamma);

    return float4(gammaCorrected, 1.0f);
}

float3 CalcDirLight(DirectionalLightData light, float3 normal, float3 viewDir, float2 uv)
{
    float3 albedo = float3(1.0f, 1.0f, 1.0f);
    float3 specMap = float3(1.0f, 1.0f, 1.0f);
    if (materialData.numDiffuse > 0)
    {
        albedo = gTextures[materialData.diffuseStartIndex].Sample(g_sampler, uv).rgb;
    }

    if (materialData.numSpecular > 0)
    {
        specMap = gTextures[materialData.specularStartIndex].Sample(g_sampler, uv).rgb;
    }

    float3 lightDir = normalize(-light.direction.xyz);
    float diff = max(dot(normal, lightDir), 0.0f);

    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0f), 0.6f * 128.0f);

    float3 ambient = light.ambient.rgb * albedo;
    float3 diffuse = light.diffuse.rgb * diff * albedo;
    float3 specular = light.specular.rgb * spec * specMap;

    return ambient + diffuse + specular;
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
