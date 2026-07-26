// 单源 bloom 降采样 + 亮通滤波（D-004 推广第一站）。
// 语义基准：Metal BloomDownsample 内核（参考路径）——4 点半源像素偏移的手写
// 双线性取均值，Threshold>0 时 smoothstep 亮通。常量为 32 字节双 texel 布局：
// 采样偏移用 SourceTexelSize（源 uv 空间），uv 由 SV_Position×OutputTexelSize 得出
// （fragment 无法查询渲染目标尺寸）。

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

float3 BrightPass(float3 color, float threshold) {
    if (threshold <= 0.0001) return color;
    const float maximumChannel = max(color.r, max(color.g, color.b));
    return color * smoothstep(threshold, threshold * 2.0, maximumChannel);
}

float4 main(float4 position : SV_Position) : SV_Target {
    const float2 uv = position.xy * OutputTexelSize;
    const float2 halfPixel = SourceTexelSize * 0.5;
    const float3 color = (SampleBilinear(SourceTexture, uv + float2(-halfPixel.x, -halfPixel.y)) +
                          SampleBilinear(SourceTexture, uv + float2(halfPixel.x, -halfPixel.y)) +
                          SampleBilinear(SourceTexture, uv + float2(-halfPixel.x, halfPixel.y)) +
                          SampleBilinear(SourceTexture, uv + float2(halfPixel.x, halfPixel.y))) * 0.25;
    return float4(BrightPass(color, Threshold), 1.0);
}
