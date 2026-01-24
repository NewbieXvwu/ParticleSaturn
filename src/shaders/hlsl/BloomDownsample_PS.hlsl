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

 float3 BrightPass(float3 hdr)
 {
     float m = max(hdr.r, max(hdr.g, hdr.b));
     // g_Threshold<=0 时不做高光提取：避免 smoothstep(edge0==edge1) 在不同后端/驱动下出现未定义行为。
     if (g_Threshold <= 0.0001)
         return hdr;
     // 软阈值：避免硬切割带来的闪烁
     float w = smoothstep(g_Threshold, g_Threshold * 2.0, m);
     return hdr * w;
 }

float4 main(PSIn i) : SV_Target
{
    // 4-tap box（轻度抗锯齿/降采样）
    float2 halfPix = g_TexelSize * 0.5;
    float3 c0 = g_Texture.Sample(g_Texture_sampler, i.UV + float2(-halfPix.x, -halfPix.y)).rgb;
    float3 c1 = g_Texture.Sample(g_Texture_sampler, i.UV + float2( halfPix.x, -halfPix.y)).rgb;
    float3 c2 = g_Texture.Sample(g_Texture_sampler, i.UV + float2(-halfPix.x,  halfPix.y)).rgb;
    float3 c3 = g_Texture.Sample(g_Texture_sampler, i.UV + float2( halfPix.x,  halfPix.y)).rgb;
    float3 col = (c0 + c1 + c2 + c3) * 0.25;
    col = BrightPass(col);
    return float4(col, 1.0);
}
