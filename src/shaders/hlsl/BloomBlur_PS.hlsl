struct PSIn
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

Texture2D    g_Texture;
SamplerState g_Texture_sampler;

cbuffer BlurCB
{
    float2 g_TexelSize;
    float  g_Offset;
    float  g_Threshold;
};

float4 main(PSIn i) : SV_Target
{
    float2 off = g_TexelSize * (g_Offset + 0.5);
    float3 sum = g_Texture.Sample(g_Texture_sampler, i.UV + float2(-off.x,  off.y)).rgb;
    sum       += g_Texture.Sample(g_Texture_sampler, i.UV + float2( off.x,  off.y)).rgb;
    sum       += g_Texture.Sample(g_Texture_sampler, i.UV + float2( off.x, -off.y)).rgb;
    sum       += g_Texture.Sample(g_Texture_sampler, i.UV + float2(-off.x, -off.y)).rgb;
    return float4(sum * 0.25, 1.0);
}
