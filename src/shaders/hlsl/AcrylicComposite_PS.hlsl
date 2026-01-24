struct PSIn
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

Texture2D    g_Texture;
SamplerState g_Texture_sampler;

cbuffer AcrylicCB
{
    float4 g_Tint;   // rgb + baseOpacity
    float4 g_Params; // x=saturation, y=adaptive, z=darkModeFlag, w=exclusionStrength
};

float3 ApplySaturation(float3 c, float sat)
{
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float3 gray = float3(lum, lum, lum);
    return saturate(gray + (c - gray) * sat);
}

float4 main(PSIn i) : SV_Target
{
    float3 col = g_Texture.Sample(g_Texture_sampler, i.UV).rgb;
    col = ApplySaturation(col, g_Params.x);

    float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
    float a   = g_Tint.a;
    float adapt = g_Params.y;
    if (g_Params.z > 0.5) // dark
        a = saturate(a + (lum - 0.5) * adapt);
    else // light
        a = saturate(a + (0.5 - lum) * adapt);

    float3 tint = g_Tint.rgb;
    float3 excl = (col + tint) - (2.0 * col * tint);
    float3 mixed = lerp(col, excl, saturate(g_Params.w));

    float3 outCol = lerp(col, mixed, a);
    return float4(outCol, 1.0);
}
