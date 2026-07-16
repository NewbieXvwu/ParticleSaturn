#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomStrength;
uniform float uTransparent;

vec3 sampleBilinear(sampler2D source, vec2 uv) {
    ivec2 size = textureSize(source, 0);
    vec2 coordinate = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(coordinate));
    vec2 fraction = fract(coordinate);
    ivec2 maximum = size - ivec2(1);
    vec3 c00 = texelFetch(source, clamp(base, ivec2(0), maximum), 0).rgb;
    vec3 c10 = texelFetch(source, clamp(base + ivec2(1, 0), ivec2(0), maximum), 0).rgb;
    vec3 c01 = texelFetch(source, clamp(base + ivec2(0, 1), ivec2(0), maximum), 0).rgb;
    vec3 c11 = texelFetch(source, clamp(base + ivec2(1, 1), ivec2(0), maximum), 0).rgb;
    return mix(mix(c00, c10, fraction.x), mix(c01, c11, fraction.x), fraction.y);
}

void main() {
    // Metal 对全分辨率 HDR 场景使用逐像素 read。这里必须保持同样的
    // 采样语义，否则线性过滤产生的微小误差会在 1.0 硬阈值附近形成分区。
    ivec2 scenePixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), textureSize(uScene, 0) - ivec2(1));
    vec3 color = texelFetch(uScene, scenePixel, 0).rgb + sampleBilinear(uBloom, vTexCoord) * uBloomStrength;
    float maximum = max(color.r, max(color.g, color.b));
    if (maximum >= 1.0) color = mix(color, color / (color + vec3(1.0)), 0.5);
    float alpha = mix(1.0, maximum, uTransparent);
    alpha = clamp(alpha, 0.0, 1.0);
    outColor = vec4(color * alpha, alpha);
}
