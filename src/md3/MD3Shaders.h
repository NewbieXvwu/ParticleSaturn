#pragma once

// MD3 Shaders - Material Design 3 效果着色器源码

namespace MD3Shaders {

// Ripple 涟漪效果顶点着色器
const char* const VertexRipple = R"(
#version 430 core
layout (location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Ripple 涟漪效果片段着色器
// 实现 MD3 规范的涟漪扩散效果，带圆角裁剪
const char* const FragmentRipple = R"(
#version 430 core

out vec4 fragColor;

uniform vec2 uRippleCenter;   // 涟漪中心 (屏幕坐标)
uniform float uRippleRadius;  // 当前半径
uniform float uRippleAlpha;   // 当前透明度
uniform vec4 uRippleColor;    // 涟漪颜色
uniform vec4 uBounds;         // 控件边界 (x, y, w, h)
uniform float uCornerRadius;  // 圆角半径
uniform vec2 uScreenSize;     // 屏幕尺寸

// 圆角矩形 SDF (Signed Distance Field)
float roundedRectSDF(vec2 p, vec2 center, vec2 halfSize, float radius) {
    vec2 d = abs(p - center) - halfSize + radius;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;
}

void main() {
    vec2 fragPos = gl_FragCoord.xy;

    // 圆角裁剪
    vec2 rectCenter = uBounds.xy + uBounds.zw * 0.5;
    float sdf = roundedRectSDF(fragPos, rectCenter, uBounds.zw * 0.5, uCornerRadius);
    if (sdf > 0.0) discard;

    float dist = distance(fragPos, uRippleCenter);

    // 软边缘 - 边缘羽化
    float edgeWidth = max(20.0, uRippleRadius * 0.15);
    float edge = smoothstep(uRippleRadius, uRippleRadius - edgeWidth, dist);

    // 中心淡出
    float fade = 1.0 - smoothstep(0.0, uRippleRadius, dist) * 0.3;

    // 抗锯齿边缘
    float aa = smoothstep(0.0, -1.0, sdf);

    fragColor = vec4(uRippleColor.rgb, uRippleColor.a * uRippleAlpha * edge * fade * aa);
}
)";

// Ripple 实例化渲染顶点着色器 (支持多个涟漪)
const char* const VertexRippleInstanced = R"(
#version 430 core
layout (location = 0) in vec2 aPos;

// 实例数据
struct RippleInstance {
    vec4 bounds;        // x, y, w, h
    vec4 centerRadius;  // centerX, centerY, radius, maxRadius
    vec4 colorAlpha;    // r, g, b, alpha
    float cornerRadius;
    float _pad[3];
};

layout(std430, binding = 2) readonly buffer RippleBuffer {
    RippleInstance ripples[];
};

out vec2 vUV;
flat out int vInstanceID;

void main() {
    vInstanceID = gl_InstanceID;
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Ripple 实例化渲染片段着色器
const char* const FragmentRippleInstanced = R"(
#version 430 core

out vec4 fragColor;
in vec2 vUV;
flat in int vInstanceID;

struct RippleInstance {
    vec4 bounds;
    vec4 centerRadius;
    vec4 colorAlpha;
    float cornerRadius;
    float _pad[3];
};

layout(std430, binding = 2) readonly buffer RippleBuffer {
    RippleInstance ripples[];
};

uniform vec2 uScreenSize;

float roundedRectSDF(vec2 p, vec2 center, vec2 halfSize, float radius) {
    vec2 d = abs(p - center) - halfSize + radius;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;
}

void main() {
    RippleInstance r = ripples[vInstanceID];
    vec2 fragPos = gl_FragCoord.xy;

    // 边界裁剪
    vec2 rectCenter = r.bounds.xy + r.bounds.zw * 0.5;
    float sdf = roundedRectSDF(fragPos, rectCenter, r.bounds.zw * 0.5, r.cornerRadius);
    if (sdf > 0.0) discard;

    vec2 center = r.centerRadius.xy;
    float radius = r.centerRadius.z;
    float dist = distance(fragPos, center);

    float edgeWidth = max(20.0, radius * 0.15);
    float edge = smoothstep(radius, radius - edgeWidth, dist);
    float fade = 1.0 - smoothstep(0.0, radius, dist) * 0.3;
    float aa = smoothstep(0.0, -1.0, sdf);

    fragColor = vec4(r.colorAlpha.rgb, r.colorAlpha.a * edge * fade * aa);
}
)";

} // namespace MD3Shaders
