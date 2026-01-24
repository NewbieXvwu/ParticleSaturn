struct PSIn
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

Texture2D    g_Texture;
SamplerState g_Texture_sampler;

Texture2D    g_BloomTexture;
SamplerState g_BloomTexture_sampler;

cbuffer BloomCB
{
    float g_BloomStrength;
    float g_Transparent;
    float g_IsD3D11; // 1.0 = D3D11 (Manual SRGB), 0.0 = D3D12/Vk (HW SRGB)
    float _pad;
};

float3 ToneMap(float3 hdr)
{
    return hdr / (hdr + float3(1.0, 1.0, 1.0));
}

// 精确的线性到 sRGB 转换（IEC 61966-2-1）
float3 LinearToSRGB(float3 color)
{
    float3 srgbLow = color * 12.92;
    float3 srgbHigh = (pow(abs(color), 1.0/2.4) * 1.055) - 0.055;
    // 逐分量选择
    float3 srgb;
    srgb.r = (color.r <= 0.0031308) ? srgbLow.r : srgbHigh.r;
    srgb.g = (color.g <= 0.0031308) ? srgbLow.g : srgbHigh.g;
    srgb.b = (color.b <= 0.0031308) ? srgbLow.b : srgbHigh.b;
    return srgb;
}

float4 main(PSIn i) : SV_Target
{
    float3 col = g_Texture.Sample(g_Texture_sampler, i.UV).rgb;
    float3 bloom = g_BloomTexture.Sample(g_BloomTexture_sampler, i.UV).rgb;

    // Bloom: 叠加模糊后的高光
    col = col + bloom * g_BloomStrength;

    // 复刻 OpenGL FragmentQuad：轻度 tone mapping（只对高光部分做压缩，强度 0.5）。
    float maxRGB = max(col.r, max(col.g, col.b));
    float w = (maxRGB >= 1.0) ? 0.5 : 0.0;
    col = lerp(col, ToneMap(col), w);

    float alpha = lerp(1.0, maxRGB, g_Transparent);

    // 如果是 D3D11，因为 SwapChain 不支持 sRGB 格式，需要手动应用 Gamma 校正
    if (g_IsD3D11 > 0.5)
    {
        col = LinearToSRGB(col);
    }

    // DirectComposition/DWM 要求预乘 alpha（premultiplied alpha）
    return float4(col * alpha, alpha);
}
