// 单源 tonemap（D-004 试点，2026-07-26 拍板 DXC + SPIRV-Cross）。
// 语义基准：src/shaders/glsl410/ToneMap.frag 与 msl ToneMapWithBloom 的共同算法
// ——场景逐像素 Load（避免线性过滤在 1.0 硬阈值附近产生分区）、bloom 手写双线性
// （全程 Load，无采样器对象，翻译面最小）、仅压缩 HDR 高光、透明模式预乘 alpha。
// 产物：DXC -T ps_6_0 → SPIR-V → SPIRV-Cross → GLSL 410 / MSL。

Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> BloomTexture : register(t1);

cbuffer ToneMapConstants : register(b0) {
    float BloomStrength;
    float Transparent;
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
    float sceneWidth;
    float sceneHeight;
    SceneTexture.GetDimensions(sceneWidth, sceneHeight);
    const int2 maximum = int2(sceneWidth, sceneHeight) - int2(1, 1);
    const int2 scenePixel = clamp(int2(position.xy), int2(0, 0), maximum);
    const float2 uv = (float2(scenePixel) + 0.5) / float2(sceneWidth, sceneHeight);
    float3 color = SceneTexture.Load(int3(scenePixel, 0)).rgb + SampleBilinear(BloomTexture, uv) * BloomStrength;
    const float maximumChannel = max(color.r, max(color.g, color.b));
    const float highlightWeight = maximumChannel >= 1.0 ? 0.5 : 0.0;
    const float3 compressed = color / (color + float3(1.0, 1.0, 1.0));
    color = lerp(color, compressed, highlightWeight);
    const float alpha = clamp(lerp(1.0, maximumChannel, Transparent), 0.0, 1.0);
    return float4(color * alpha, alpha);
}
