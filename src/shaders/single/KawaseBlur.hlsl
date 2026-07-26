// 单源 Kawase 模糊（D-004 推广第一站）。
// 语义基准：Metal KawaseBlur 内核（参考路径）——4 点对角偏移的手写双线性取均值，
// 偏移距离 = SourceTexelSize × (Offset + 0.5)。常量布局与 BloomDownsample 共用
// （32 字节双 texel；模糊 ping-pong 源目标同尺寸，两个 texel 相等）。

Texture2D<float4> SourceTexture : register(t0);

cbuffer BloomConstants : register(b0) {
    float2 SourceTexelSize;
    float2 OutputTexelSize;
    float Offset;
    float Threshold;
    float2 Padding;
};

float3 SampleBilinear(Texture2D<float4> source, float2 uv) {
    float width;
    float height;
    source.GetDimensions(width, height);
    const float2 size = float2(width, height);
    const float2 coordinate = uv * size - 0.5;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = frac(coordinate);
    const int2 maximum = int2(size) - int2(1, 1);
    const float3 c00 = source.Load(int3(clamp(base, int2(0, 0), maximum), 0)).rgb;
    const float3 c10 = source.Load(int3(clamp(base + int2(1, 0), int2(0, 0), maximum), 0)).rgb;
    const float3 c01 = source.Load(int3(clamp(base + int2(0, 1), int2(0, 0), maximum), 0)).rgb;
    const float3 c11 = source.Load(int3(clamp(base + int2(1, 1), int2(0, 0), maximum), 0)).rgb;
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

float4 main(float4 position : SV_Position) : SV_Target {
    const float2 uv = position.xy * OutputTexelSize;
    const float2 offset = SourceTexelSize * (Offset + 0.5);
    const float3 color = (SampleBilinear(SourceTexture, uv + float2(-offset.x, -offset.y)) +
                          SampleBilinear(SourceTexture, uv + float2(offset.x, -offset.y)) +
                          SampleBilinear(SourceTexture, uv + float2(-offset.x, offset.y)) +
                          SampleBilinear(SourceTexture, uv + float2(offset.x, offset.y))) * 0.25;
    return float4(color, 1.0);
}
