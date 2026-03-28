// Constant Buffer
struct SceneData
{
    float4x4 mvp;
};
ConstantBuffer<SceneData> cb : register(b0);

struct VSInput
{
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float3 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput vout;
    vout.pos = mul(float4(input.pos, 1.0f), cb.mvp);
    // Debug bằng cách lấy Normal làm màu
    vout.color = input.norm * 0.5f + 0.5f;
    return vout;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}