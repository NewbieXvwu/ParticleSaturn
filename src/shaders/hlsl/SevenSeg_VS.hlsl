cbuffer SevenSegCB : register(b0)
{
    float4x4 Projection;
    float4 Transform; // x, y, scaleX, scaleY
    float3 Color;
    float _pad;
};

struct VSOut
{
    float4 Pos : SV_POSITION;
    float3 Col : COLOR;
};

VSOut main(float2 inPos : ATTRIB0)
{
    // 应用变换: position = (inPos * scale) + offset
    float2 worldPos = inPos * Transform.zw + Transform.xy;
    VSOut o;
    o.Pos = mul(Projection, float4(worldPos, 0.0, 1.0));
    o.Col = Color;
    return o;
}
