#include "DiligentShaderSources.h"

namespace ParticleSaturn::Render {

using namespace Diligent;

ShaderSources GetFullscreenQuadShaderSources(Backend backend) {
    // 目标：把离屏 HDR 结果（R11G11B10F）通过全屏四边形合成到 SwapChain。
    // 这一步在 OpenGL 旧版相当于 FragmentQuad（轻度 tone mapping），
    // 能显著降低“加法混合导致的过曝/泛白”。
    static constexpr char kHlslVS[] = R"(
struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

VSOut main(uint VertID : SV_VertexID)
{
    float2 pos[4] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };

    float2 uv[4] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut o;
    o.Pos = float4(pos[VertID], 0.0, 1.0);
    o.UV  = uv[VertID];
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
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
    float3 srgb = (color <= 0.0031308) ? srgbLow : srgbHigh;
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
)";

    static constexpr char kGlslVS[] = R"(
layout(location = 0) out vec2 vUV;

void main()
{
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0)
    );

    const vec2 uv[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0)
    );

    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV         = uv[gl_VertexIndex];
}
)";

    static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// Vulkan 下显式 set/binding，避免不同驱动默认绑定行为差异。
layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(set=0, binding=1) uniform sampler2D g_BloomTexture;

layout(std140, set=0, binding=2) uniform BloomCB
{
    float g_BloomStrength;
    float g_Transparent;
    vec2 _pad;
};

vec3 toneMap(vec3 hdr)
{
    return hdr / (hdr + vec3(1.0));
}

void main()
{
    vec3 col = texture(g_Texture, vUV).rgb;
    vec3 bloom = texture(g_BloomTexture, vUV).rgb;

    // Bloom: 叠加模糊后的高光
    col = col + bloom * g_BloomStrength;

    float maxRGB = max(max(col.r, col.g), col.b);
    float w = (maxRGB >= 1.0) ? 0.5 : 0.0;
    col = mix(col, toneMap(col), w);

    float alpha = mix(1.0, maxRGB, g_Transparent);
    // DirectComposition/DWM 要求预乘 alpha（premultiplied alpha）
    oColor = vec4(col * alpha, alpha);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ShaderSources GetBloomDownsampleShaderSources(Backend backend) {
    // Bright-pass + downsample：把 HDR 场景提取成低分辨率 bloom 源（只保留高光）。
    //
    // 常量：
    // - g_TexelSize : 源纹理 texel size（1/srcW, 1/srcH）
    // - g_Threshold : 高光阈值（HDR 值，典型 1.0）
    static constexpr char kHlslVS[] = R"(
struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

VSOut main(uint VertID : SV_VertexID)
{
    float2 pos[4] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };

    float2 uv[4] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut o;
    o.Pos = float4(pos[VertID], 0.0, 1.0);
    o.UV  = uv[VertID];
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
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
)";

    static constexpr char kGlslVS[] = R"(
layout(location = 0) out vec2 vUV;
void main()
{
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0)
    );
    const vec2 uv[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV = uv[gl_VertexIndex];
}
)";

    static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(std140, set=0, binding=1) uniform BlurCB
{
    vec2  g_TexelSize;
    float g_Offset;
    float g_Threshold;
};

 vec3 brightPass(vec3 hdr)
 {
     float m = max(hdr.r, max(hdr.g, hdr.b));
     // g_Threshold<=0 时不做高光提取：避免 smoothstep(edge0==edge1) 的未定义行为。
     if (g_Threshold <= 0.0001)
         return hdr;
     float w = smoothstep(g_Threshold, g_Threshold * 2.0, m);
     return hdr * w;
 }

void main()
{
    vec2 halfPix = g_TexelSize * 0.5;
    vec3 c0 = texture(g_Texture, vUV + vec2(-halfPix.x, -halfPix.y)).rgb;
    vec3 c1 = texture(g_Texture, vUV + vec2( halfPix.x, -halfPix.y)).rgb;
    vec3 c2 = texture(g_Texture, vUV + vec2(-halfPix.x,  halfPix.y)).rgb;
    vec3 c3 = texture(g_Texture, vUV + vec2( halfPix.x,  halfPix.y)).rgb;
    vec3 col = (c0 + c1 + c2 + c3) * 0.25;
    col = brightPass(col);
    oColor = vec4(col, 1.0);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ShaderSources GetBloomBlurShaderSources(Backend backend) {
    // Kawase blur：4-tap 对角采样（offset 随迭代递增）
    static constexpr char kHlslVS[] = R"(
struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

VSOut main(uint VertID : SV_VertexID)
{
    float2 pos[4] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };

    float2 uv[4] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut o;
    o.Pos = float4(pos[VertID], 0.0, 1.0);
    o.UV  = uv[VertID];
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
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
)";

    static constexpr char kGlslVS[] = R"(
layout(location = 0) out vec2 vUV;
void main()
{
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0)
    );
    const vec2 uv[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV = uv[gl_VertexIndex];
}
)";

    static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(std140, set=0, binding=1) uniform BlurCB
{
    vec2  g_TexelSize;
    float g_Offset;
    float g_Threshold;
};

void main()
{
    vec2 off = g_TexelSize * (g_Offset + 0.5);
    vec3 sum = texture(g_Texture, vUV + vec2(-off.x,  off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2( off.x,  off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2( off.x, -off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2(-off.x, -off.y)).rgb;
    oColor = vec4(sum * 0.25, 1.0);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ShaderSources GetAcrylicCompositeShaderSources(Backend backend) {
    // Acrylic 近似合成（低分辨率）：饱和度增强 + 近似 exclusion + tint（带亮度自适应）
    static constexpr char kHlslVS[] = R"(
struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

VSOut main(uint VertID : SV_VertexID)
{
    float2 pos[4] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };

    float2 uv[4] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut o;
    o.Pos = float4(pos[VertID], 0.0, 1.0);
    o.UV  = uv[VertID];
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
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
)";

    static constexpr char kGlslVS[] = R"(
layout(location = 0) out vec2 vUV;
void main()
{
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0)
    );
    const vec2 uv[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV = uv[gl_VertexIndex];
}
)";

    static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(std140, set=0, binding=1) uniform AcrylicCB
{
    vec4 g_Tint;   // rgb + baseOpacity
    vec4 g_Params; // x=saturation, y=adaptive, z=darkModeFlag, w=exclusionStrength
};

vec3 applySaturation(vec3 c, float sat)
{
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 gray = vec3(lum);
    return clamp(gray + (c - gray) * sat, 0.0, 1.0);
}

void main()
{
    vec3 col = texture(g_Texture, vUV).rgb;
    col = applySaturation(col, g_Params.x);

    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float a   = g_Tint.a;
    float adapt = g_Params.y;
    if (g_Params.z > 0.5) // dark
        a = clamp(a + (lum - 0.5) * adapt, 0.0, 1.0);
    else // light
        a = clamp(a + (0.5 - lum) * adapt, 0.0, 1.0);

    vec3 tint = g_Tint.rgb;
    vec3 excl = (col + tint) - (2.0 * col * tint);
    vec3 mixed = mix(col, excl, clamp(g_Params.w, 0.0, 1.0));
    vec3 outCol = mix(col, mixed, a);

    oColor = vec4(outCol, 1.0);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ComputeShaderSource GetSaturnComputeShaderSource(Backend backend) {
    // 1:1 复刻 OpenGL ComputeSaturn（粒子物理模拟，双缓冲逻辑），但这里会接入三缓冲轮转：
    // - in  = 当前 render buffer
    // - out = write buffer
    // - dispatch 后交换索引，让 out 成为新的 render buffer
    //
    // 注意：shader 逻辑仍然是“读一个 buffer，写一个 buffer”，与 OpenGL 完全一致。

    static constexpr char kHlslCS[] = R"(
struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

StructuredBuffer<ParticleData>   g_ParticlesIn;
RWStructuredBuffer<ParticleData> g_ParticlesOut;

cbuffer ComputeConstants
{
    float uDt;
    float uHandScale;
    float uHandHas;
    uint  uParticleCount;
};

groupshared float s_timeFactor;
groupshared float s_bodyAngleCos;
groupshared float s_bodyAngleSin;
groupshared float s_dtScaled;

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    uint id = DTid.x;

    if (GTid.x == 0)
    {
        s_timeFactor   = lerp(1.0, uHandScale, uHandHas);
        float bodyAngle = 0.03 * uDt * s_timeFactor;
        s_bodyAngleCos = cos(bodyAngle);
        s_bodyAngleSin = sin(bodyAngle);
        s_dtScaled     = 0.2 * uDt * s_timeFactor;
    }
    GroupMemoryBarrierWithGroupSync();

    if (id >= uParticleCount)
        return;

    ParticleData p = g_ParticlesIn[id];

    float c, s;
    if (p.isRing < 0.5)
    {
        c = s_bodyAngleCos;
        s = s_bodyAngleSin;
    }
    else
    {
        float angle = p.speed * s_dtScaled;
        c = cos(angle);
        s = sin(angle);
    }

    g_ParticlesOut[id].pos.x = p.pos.x * c - p.pos.z * s;
    g_ParticlesOut[id].pos.y = p.pos.y;
    g_ParticlesOut[id].pos.z = p.pos.x * s + p.pos.z * c;
    g_ParticlesOut[id].pos.w = p.pos.w;
    g_ParticlesOut[id].color = p.color;
    g_ParticlesOut[id].speed = p.speed;
    g_ParticlesOut[id].isRing = p.isRing;
    g_ParticlesOut[id].pad = p.pad;
}
)";

    static constexpr char kGlslCS[] = R"(
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

struct ParticleData
{
    vec4  pos;
    uint  color;
    float speed;
    float isRing;
    float pad;
};

layout(set=0, binding=0, std430) readonly buffer g_ParticlesIn
{
    ParticleData particlesIn[];
};

layout(set=0, binding=1, std430) writeonly buffer g_ParticlesOut
{
    ParticleData particlesOut[];
};

layout(set=0, binding=2, std140) uniform ComputeConstants
{
    float uDt;
    float uHandScale;
    float uHandHas;
    uint  uParticleCount;
};

shared float s_timeFactor;
shared float s_bodyAngleCos;
shared float s_bodyAngleSin;
shared float s_dtScaled;

void main()
{
    uint id = gl_GlobalInvocationID.x;

    if (gl_LocalInvocationID.x == 0u)
    {
        s_timeFactor    = mix(1.0, uHandScale, uHandHas);
        float bodyAngle = 0.03 * uDt * s_timeFactor;
        s_bodyAngleCos  = cos(bodyAngle);
        s_bodyAngleSin  = sin(bodyAngle);
        s_dtScaled      = 0.2 * uDt * s_timeFactor;
    }
    barrier();

    if (id >= uParticleCount)
        return;

    ParticleData p = particlesIn[id];

    float c, s;
    if (p.isRing < 0.5)
    {
        c = s_bodyAngleCos;
        s = s_bodyAngleSin;
    }
    else
    {
        float angle = p.speed * s_dtScaled;
        c = cos(angle);
        s = sin(angle);
    }

    particlesOut[id].pos.x = p.pos.x * c - p.pos.z * s;
    particlesOut[id].pos.y = p.pos.y;
    particlesOut[id].pos.z = p.pos.x * s + p.pos.z * c;
    particlesOut[id].pos.w = p.pos.w;
    particlesOut[id].color = p.color;
    particlesOut[id].speed = p.speed;
    particlesOut[id].isRing = p.isRing;
    particlesOut[id].pad = p.pad;
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslCS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslCS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ComputeShaderSource GetSaturnInitComputeShaderSource(Backend backend) {
    // 粒子初始化 Compute Shader：复刻 InitSaturnParticlesCPU 的逻辑
    // 在 GPU 上并行初始化所有粒子，避免 CPU 到 GPU 的大量数据传输
    //
    // 常量：
    // - uParticleCount: 粒子总数
    // - uSeed: 随机种子（通常是 time(0)）
    // - uRadius: 土星半径（默认 18.0）

    static constexpr char kHlslCS[] = R"(
struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

RWStructuredBuffer<ParticleData> g_ParticlesOut;

cbuffer InitConstants
{
    uint  uParticleCount;
    uint  uSeed;
    float uRadius;
    float _pad;
};

// PCG 伪随机数生成器
uint PCGHash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random01(inout uint state)
{
    state = PCGHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

uint PackRGBA8(float r, float g, float b, float a)
{
    uint ir = uint(saturate(r) * 255.0 + 0.5);
    uint ig = uint(saturate(g) * 255.0 + 0.5);
    uint ib = uint(saturate(b) * 255.0 + 0.5);
    uint ia = uint(saturate(a) * 255.0 + 0.5);
    return ir | (ig << 8) | (ib << 16) | (ia << 24);
}

float3 HexToRGB(uint hex)
{
    float r = float((hex >> 16) & 0xFF) / 255.0;
    float g = float((hex >> 8) & 0xFF) / 255.0;
    float b = float(hex & 0xFF) / 255.0;
    return float3(r, g, b);
}

float3 Mix(float3 a, float3 b, float t)
{
    return a * (1.0 - t) + b * t;
}

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint id = DTid.x;
    if (id >= uParticleCount)
        return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;
    float typeRnd = Random01(rngState);

    float4 pos = float4(0, 0, 0, 1);
    float3 colRGB = float3(1, 1, 1);
    float alpha = 1.0;
    float speed = 0.0;
    float isRing = 0.0;

    float R = uRadius;

    if (typeRnd < 0.25)
    {
        // --- 土星本体粒子 ---
        float th = 6.28318 * Random01(rngState);
        float ph = acos(2.0 * Random01(rngState) - 1.0);

        pos.x = R * sin(ph) * cos(th);
        pos.y = R * cos(ph) * 0.9;
        pos.z = R * sin(ph) * sin(th);

        float lat = (pos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4;
        if (ci < 0) ci = 0;

        float3 cols[4];
        cols[0] = HexToRGB(0xE3DAC5);
        cols[1] = HexToRGB(0xC9A070);
        cols[2] = HexToRGB(0xE3DAC5);
        cols[3] = HexToRGB(0xB08D55);
        colRGB = cols[ci];

        pos.w = 1.0 + Random01(rngState) * 0.8;
        alpha = 0.8;
        speed = 0.0;
        isRing = 0.0;
    }
    else
    {
        // --- 土星环粒子 ---
        float z = Random01(rngState);
        float rad = 0.0;
        float3 c = float3(1, 1, 1);
        float s = 1.0;
        float o = 1.0;

        if (z < 0.15)
        {
            rad = R * (1.235 + Random01(rngState) * 0.29);
            c = HexToRGB(0x2A2520);
            s = 0.5;
            o = 0.3;
        }
        else if (z < 0.65)
        {
            float t = Random01(rngState);
            rad = R * (1.525 + t * 0.425);
            c = Mix(HexToRGB(0xCDBFA0), HexToRGB(0xDCCBBA), t);
            s = 0.8 + Random01(rngState) * 0.6;
            o = 0.85;
            if (sin(rad * 2.0) > 0.8)
                o *= 1.2;
        }
        else if (z < 0.69)
        {
            rad = R * (1.95 + Random01(rngState) * 0.075);
            c = HexToRGB(0x050505);
            s = 0.3;
            o = 0.1;
        }
        else if (z < 0.99)
        {
            rad = R * (2.025 + Random01(rngState) * 0.245);
            c = HexToRGB(0x989085);
            s = 0.7;
            o = 0.6;
            if (rad > R * 2.2 && rad < R * 2.21)
                o = 0.1;
        }
        else
        {
            rad = R * (2.32 + Random01(rngState) * 0.02);
            c = HexToRGB(0xAFAFA0);
            s = 1.0;
            o = 0.7;
        }

        float th = Random01(rngState) * 6.28318;
        pos.x = rad * cos(th);
        pos.z = rad * sin(th);

        float heightRange = (rad > R * 2.3) ? 0.4 : 0.15;
        pos.y = (Random01(rngState) - 0.5) * heightRange;

        colRGB = c;
        pos.w = s;
        alpha = o;
        speed = 8.0 / sqrt(rad);
        isRing = 1.0;
    }

    ParticleData p;
    p.pos = pos;
    p.color = PackRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.pad = 0.0;
    g_ParticlesOut[id] = p;
}
)";

    static constexpr char kGlslCS[] = R"(
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

struct ParticleData
{
    vec4  pos;
    uint  color;
    float speed;
    float isRing;
    float pad;
};

layout(set=0, binding=0, std430) writeonly buffer g_ParticlesOut
{
    ParticleData particlesOut[];
};

layout(set=0, binding=1, std140) uniform InitConstants
{
    uint  uParticleCount;
    uint  uSeed;
    float uRadius;
    float _pad;
};

// PCG 伪随机数生成器
uint pcgHash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random01(inout uint state)
{
    state = pcgHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

uint packRGBA8(float r, float g, float b, float a)
{
    uint ir = uint(clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    uint ig = uint(clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    uint ib = uint(clamp(b, 0.0, 1.0) * 255.0 + 0.5);
    uint ia = uint(clamp(a, 0.0, 1.0) * 255.0 + 0.5);
    return ir | (ig << 8u) | (ib << 16u) | (ia << 24u);
}

vec3 hexToRGB(uint hex)
{
    float r = float((hex >> 16u) & 0xFFu) / 255.0;
    float g = float((hex >> 8u) & 0xFFu) / 255.0;
    float b = float(hex & 0xFFu) / 255.0;
    return vec3(r, g, b);
}

void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount)
        return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;
    float typeRnd = random01(rngState);

    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);
    vec3 colRGB = vec3(1.0);
    float alpha = 1.0;
    float speed = 0.0;
    float isRing = 0.0;

    float R = uRadius;

    if (typeRnd < 0.25)
    {
        // --- 土星本体粒子 ---
        float th = 6.28318 * random01(rngState);
        float ph = acos(2.0 * random01(rngState) - 1.0);

        pos.x = R * sin(ph) * cos(th);
        pos.y = R * cos(ph) * 0.9;
        pos.z = R * sin(ph) * sin(th);

        float lat = (pos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4;
        if (ci < 0) ci = 0;

        vec3 cols[4];
        cols[0] = hexToRGB(0xE3DAC5u);
        cols[1] = hexToRGB(0xC9A070u);
        cols[2] = hexToRGB(0xE3DAC5u);
        cols[3] = hexToRGB(0xB08D55u);
        colRGB = cols[ci];

        pos.w = 1.0 + random01(rngState) * 0.8;
        alpha = 0.8;
        speed = 0.0;
        isRing = 0.0;
    }
    else
    {
        // --- 土星环粒子 ---
        float z = random01(rngState);
        float rad = 0.0;
        vec3 c = vec3(1.0);
        float s = 1.0;
        float o = 1.0;

        if (z < 0.15)
        {
            rad = R * (1.235 + random01(rngState) * 0.29);
            c = hexToRGB(0x2A2520u);
            s = 0.5;
            o = 0.3;
        }
        else if (z < 0.65)
        {
            float t = random01(rngState);
            rad = R * (1.525 + t * 0.425);
            c = mix(hexToRGB(0xCDBFA0u), hexToRGB(0xDCCBBAu), t);
            s = 0.8 + random01(rngState) * 0.6;
            o = 0.85;
            if (sin(rad * 2.0) > 0.8)
                o *= 1.2;
        }
        else if (z < 0.69)
        {
            rad = R * (1.95 + random01(rngState) * 0.075);
            c = hexToRGB(0x050505u);
            s = 0.3;
            o = 0.1;
        }
        else if (z < 0.99)
        {
            rad = R * (2.025 + random01(rngState) * 0.245);
            c = hexToRGB(0x989085u);
            s = 0.7;
            o = 0.6;
            if (rad > R * 2.2 && rad < R * 2.21)
                o = 0.1;
        }
        else
        {
            rad = R * (2.32 + random01(rngState) * 0.02);
            c = hexToRGB(0xAFAFA0u);
            s = 1.0;
            o = 0.7;
        }

        float th = random01(rngState) * 6.28318;
        pos.x = rad * cos(th);
        pos.z = rad * sin(th);

        float heightRange = (rad > R * 2.3) ? 0.4 : 0.15;
        pos.y = (random01(rngState) - 0.5) * heightRange;

        colRGB = c;
        pos.w = s;
        alpha = o;
        speed = 8.0 / sqrt(rad);
        isRing = 1.0;
    }

    ParticleData p;
    p.pos = pos;
    p.color = packRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.pad = 0.0;
    particlesOut[id] = p;
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslCS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslCS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ShaderSources GetStarShaderSources(Backend backend) {
    // 尽量复刻 OpenGL 旧实现的视觉逻辑：
    // - 星星在 3D 球壳上分布（CPU 预生成）
    // - Vertex：view/proj/model 变换，点大小随深度变化（旧实现：aSize*(1000/-p.z)）
    // - Fragment：圆形软边（点精灵），基于 gl_FragCoord/SV_Position 的闪烁噪声
    //
    // 为了让 D3D12/Vulkan 都能得到圆形点精灵效果，这里用“实例化四边形 billboard”实现点精灵：
    // 每个星星一个 instance，绘制 2 个三角形（6 顶点），fragment 用 UV 做圆形裁剪。

    static constexpr char kHlslVS[] = R"(
cbuffer StarConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    // x = 2/width, y = 2/height
    float4 uViewportParams;

    // x = timeSeconds
    float4 uTimeParams;
};

struct VSIn
{
    float3 Pos   : ATTRIB0; // world
    float3 Color : ATTRIB1;
    float  Size  : ATTRIB2;
    float  Seed  : ATTRIB3;
};

struct VSOut
{
    float4 Pos   : SV_POSITION;
    float3 Color : COLOR0;
    float  Seed  : TEXCOORD0;
    float2 UV    : TEXCOORD1;
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

VSOut main(uint VertId : SV_VertexID, VSIn i)
{
    // 两个三角形拼一个 quad（以中心为 anchor 的 billboard）
    float2 corners[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),

        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0)
    };

    float2 corner = corners[VertId % 6];
    float2 uv     = corner * 0.5 + 0.5;

    float4 worldPos = MulRows(float4(i.Pos, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 viewPos  = MulRows(worldPos,            uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);
    float4 clipPos  = MulRows(viewPos,             uProjRow0,  uProjRow1,  uProjRow2,  uProjRow3);

    // OpenGL 旧逻辑假设：相机前方 viewPos.z < 0，使用 -viewPos.z 做深度缩放。
    // 为了避免“在相机后方的点”因为 clamp 导致尺寸异常，直接裁剪掉。
    if (viewPos.z >= -0.001)
    {
        VSOut o;
        o.Pos   = float4(2.0, 2.0, 2.0, 1.0); // 保证被裁剪
        o.Color = float3(0.0, 0.0, 0.0);
        o.Seed  = 0.0;
        o.UV    = float2(0.0, 0.0);
        return o;
    }

    float invZ = 1.0 / (-viewPos.z);
    float px   = clamp(i.Size * (1000.0 * invZ), 1.0, 8.0);

    // 从像素尺寸转换到 NDC 偏移，再转回 clip space（保持屏幕空间恒定大小）。
    float2 ndcCenter = clipPos.xy / clipPos.w;
    float2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    float2 ndcPos    = ndcCenter + ndcOffset;

    VSOut o;
    o.Pos   = float4(ndcPos * clipPos.w, clipPos.z, clipPos.w);
    o.Color = i.Color;
    o.Seed  = i.Seed;
    o.UV    = uv;
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
cbuffer StarConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    float4 uViewportParams;
    float4 uTimeParams; // x = timeSeconds
};

struct PSIn
{
    float4 Pos   : SV_POSITION;
    float3 Color : COLOR0;
    float  Seed  : TEXCOORD0;
    float2 UV    : TEXCOORD1;
};

float4 main(PSIn i) : SV_Target
{
    float2 c = 2.0 * i.UV - 1.0;
    float  rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float n  = frac(sin(dot(i.Pos.xy, float2(12.9, 78.2))) * 43758.5);
    float tw = 0.7 + 0.3 * sin(uTimeParams.x * 2.0 + (n + i.Seed) * 10.0);

    float3 col = i.Color * tw * 3.0;
    float  a   = pow(1.0 - rr, 1.5) * 0.9;
    return float4(col, a);
}
)";

    static constexpr char kGlslVS[] = R"(
layout(std140) uniform StarConstants
{
    vec4 uViewRow0;
    vec4 uViewRow1;
    vec4 uViewRow2;
    vec4 uViewRow3;

    vec4 uProjRow0;
    vec4 uProjRow1;
    vec4 uProjRow2;
    vec4 uProjRow3;

    vec4 uModelRow0;
    vec4 uModelRow1;
    vec4 uModelRow2;
    vec4 uModelRow3;

    vec4 uViewportParams; // x = 2/width, y = 2/height
    vec4 uTimeParams;     // x = timeSeconds
};

layout(location = 0) in vec3 inPos;     // world
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inSize;
layout(location = 3) in float inSeed;

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vSeed;
layout(location = 2) out vec2 vUV;

vec4 mulRows(vec4 v, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
{
    return vec4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

void main()
{
    vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2( 1.0, -1.0)
    );

    vec2 corner = corners[gl_VertexIndex % 6];
    vec2 uv     = corner * 0.5 + 0.5;

    vec4 worldPos = mulRows(vec4(inPos, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    vec4 viewPos  = mulRows(worldPos,          uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);
    vec4 clipPos  = mulRows(viewPos,           uProjRow0,  uProjRow1,  uProjRow2,  uProjRow3);

    if (viewPos.z >= -0.001)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vColor = vec3(0.0);
        vSeed  = 0.0;
        vUV    = vec2(0.0);
        return;
    }

    float invZ = 1.0 / (-viewPos.z);
    float px   = clamp(inSize * (1000.0 * invZ), 1.0, 8.0);

    vec2 ndcCenter = clipPos.xy / clipPos.w;
    vec2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    vec2 ndcPos    = ndcCenter + ndcOffset;

    gl_Position = vec4(ndcPos * clipPos.w, clipPos.z, clipPos.w);

    vColor = inColor;
    vSeed  = inSeed;
    vUV    = uv;
}
)";

    static constexpr char kGlslPS[] = R"(
layout(std140) uniform StarConstants
{
    vec4 uViewRow0;
    vec4 uViewRow1;
    vec4 uViewRow2;
    vec4 uViewRow3;

    vec4 uProjRow0;
    vec4 uProjRow1;
    vec4 uProjRow2;
    vec4 uProjRow3;

    vec4 uModelRow0;
    vec4 uModelRow1;
    vec4 uModelRow2;
    vec4 uModelRow3;

    vec4 uViewportParams;
    vec4 uTimeParams; // x = timeSeconds
};

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vSeed;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 oColor;

void main()
{
    vec2 c = 2.0 * vUV - 1.0;
    float rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float n  = fract(sin(dot(gl_FragCoord.xy, vec2(12.9, 78.2))) * 43758.5);
    float tw = 0.7 + 0.3 * sin(uTimeParams.x * 2.0 + (n + vSeed) * 10.0);

    vec3 col = vColor * tw * 3.0;
    float a  = pow(1.0 - rr, 1.5) * 0.9;
    oColor   = vec4(col, a);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

ShaderSources GetSaturnParticleShaderSources(Backend backend) {
    // 目标：尽量 1:1 复刻 OpenGL 的 VertexSaturn / FragmentSaturn 视觉逻辑，
    // 但为了跨 D3D12/Vulkan 一致性使用 billboard quad（实例化 6 顶点）来替代 gl_PointCoord。
    //
    // 对应 OpenGL 旧逻辑要点：
    // - view/projection/model 变换 + chaos（近距离混沌扰动）
    // - 点大小：aPos.w * 350 * (1/dist) * 0.55 * (uScreenHeight/1080) * ringFactor * pow(uPixelRatio,0.8)
    // - 片元：圆形裁剪 + glow + 与 scale/距离相关的颜色/透明度
    // - 混合：SrcAlpha + One（加法）

    static constexpr char kHlslVS[] = R"(
struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

StructuredBuffer<ParticleData> g_Particles;

cbuffer ParticleConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    float4 uViewportParams; // x=2/width, y=2/height, z=width, w=height
    float4 uTimeParams;     // x=time
    float4 uRenderParams;   // x=uScale, y=uPixelRatio, z=uScreenHeight, w=uDensityComp
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float4 UnpackRGBA8(uint c)
{
    float4 o;
    o.x = float((c      ) & 0xFFu) / 255.0;
    o.y = float((c >>  8) & 0xFFu) / 255.0;
    o.z = float((c >> 16) & 0xFFu) / 255.0;
    o.w = float((c >> 24) & 0xFFu) / 255.0;
    return o;
}

float SmoothStep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = saturate(t);
    return t * t * (3.0 - 2.0 * t);
}

float Hash(float n)
{
    uint x = asuint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

float FastSin(float x)
{
    const float TwoPi = 6.28318530718;
    const float Pi    = 3.14159265359;

    x = fmod(x, TwoPi);
    x = (x > Pi) ? (x - TwoPi) : x;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * (0.00833333 - x2 * 0.0001984)));
}

struct VSOut
{
    float4 Pos   : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float3 vColor       : TEXCOORD1;
    float  vDist        : TEXCOORD2;
    float  vOpacity     : TEXCOORD3;
    float  vScaleFactor : TEXCOORD4;
    float  vIsRing      : TEXCOORD5;
};

VSOut main(uint VertId : SV_VertexID, uint InstId : SV_InstanceID)
{
    float2 corners[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),

        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0)
    };

    float2 corner = corners[VertId % 6];

    ParticleData p = g_Particles[InstId];
    float4 col = UnpackRGBA8(p.color);

    const float uScale       = uRenderParams.x;
    const float uPixelRatio  = uRenderParams.y;
    const float uScreenHeight= uRenderParams.z;

    float4 worldPos = MulRows(float4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 mvPosition  = MulRows(worldPos,                    uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);

    float dist = -mvPosition.z;

    // 与 OpenGL 旧逻辑一致：只接受 viewPos.z < 0 的点（相机前方）。
    if (mvPosition.z >= -0.001)
    {
        VSOut o;
        o.Pos = float4(2.0, 2.0, 2.0, 1.0);
        o.UV  = float2(0.0, 0.0);
        o.vColor = float3(0.0, 0.0, 0.0);
        o.vDist = 0.0;
        o.vOpacity = 0.0;
        o.vScaleFactor = 0.0;
        o.vIsRing = 0.0;
        return o;
    }

    // Chaos（近距离扰动）
    float chaosThreshold = 25.0;
    float chaosIntensity = SmoothStep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    float3 noiseVec = float3(0.0, 0.0, 0.0);
    if (chaosIntensity > 0.001)
    {
        float highFreqTime = uTimeParams.x * 40.0;
        float3 posScaled   = p.pos.xyz * 10.0;
        float hashX = Hash(p.pos.y * 43758.5) * 0.5;
        float hashY = Hash(p.pos.x * 43758.5) * 0.5;
        float hashZ = Hash(p.pos.z * 43758.5) * 0.5;
        noiseVec = float3(
            FastSin(highFreqTime + posScaled.x) * hashX,
            FastSin(highFreqTime + posScaled.y + 1.5708) * hashY,
            FastSin(highFreqTime * 0.5) * hashZ
        ) * 3.0;
    }
    mvPosition.xyz = lerp(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    float4 clipPos  = MulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);

    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = p.pos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;

    // ringFactor：仅对本体粒子在近距离略缩小
    float nearMask = (dist <= 50.0) ? 1.0 : 0.0;
    float ringFactor = lerp(lerp(1.0, 0.8, nearMask), 1.0, p.isRing);
    pointSize *= ringFactor * pow(max(uPixelRatio, 0.0001), 0.8);

    float px = clamp(pointSize, 0.0, 300.0 * screenScale);

    float2 ndcCenter = clipPos.xy / clipPos.w;
    float2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    float2 ndcPos    = ndcCenter + ndcOffset;

    VSOut o;
    o.Pos   = float4(ndcPos * clipPos.w, clipPos.z, clipPos.w);
    o.UV    = corner * 0.5 + 0.5;
    o.vColor       = col.rgb;
    o.vDist        = dist;
    o.vOpacity     = col.a;
    o.vScaleFactor = uScale;
    o.vIsRing      = p.isRing;
    return o;
}
)";

    static constexpr char kHlslPS[] = R"(
struct PSIn
{
    float4 Pos   : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float3 vColor       : TEXCOORD1;
    float  vDist        : TEXCOORD2;
    float  vOpacity     : TEXCOORD3;
    float  vScaleFactor : TEXCOORD4;
    float  vIsRing      : TEXCOORD5;
};

cbuffer ParticleConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    float4 uViewportParams;
    float4 uTimeParams;
    float4 uRenderParams; // w = uDensityComp
};

float SmoothStep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = saturate(t);
    return t * t * (3.0 - 2.0 * t);
}

float4 main(PSIn i) : SV_Target
{
    float2 c = 2.0 * i.UV - 1.0;
    float  rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float glow = SmoothStep(1.0, 0.4, rr);

    float t = clamp((i.vScaleFactor - 0.15) * 0.4255, 0.0, 1.0);
    float tSmooth = SmoothStep(0.1, 0.9, t);

    float3 baseColor  = lerp(float3(0.35, 0.22, 0.05), i.vColor, tSmooth);
    float3 finalColor = baseColor * (0.2 + t);

    float closeMix = SmoothStep(40.0, 0.0, i.vDist);
    float3 closeRingColor = finalColor + float3(0.15, 0.12, 0.1) * closeMix;
    float3 closeBodyColor = lerp(finalColor, pow(i.vColor, float3(1.4, 1.4, 1.4)) * 1.5, closeMix * 0.8);
    finalColor = lerp(closeBodyColor, closeRingColor, i.vIsRing);

    float depthAlpha = SmoothStep(0.0, 10.0, i.vDist);
    float densityComp = uRenderParams.w;
    float finalAlpha = glow * i.vOpacity * (0.25 + 0.45 * SmoothStep(0.0, 0.5, t)) * depthAlpha * densityComp;

    return float4(finalColor, finalAlpha);
}
)";

    static constexpr char kGlslVS[] = R"(
struct ParticleData
{
    vec4  pos;
    uint  color;
    float speed;
    float isRing;
    float pad;
};

// Vulkan 下建议显式指定 set/binding，避免不同驱动/编译器对默认绑定行为不一致。
layout(set=0, binding=0, std430) readonly buffer g_Particles
{
    ParticleData particles[];
};

layout(set=0, binding=1, std140) uniform ParticleConstants
{
    vec4 uViewRow0;
    vec4 uViewRow1;
    vec4 uViewRow2;
    vec4 uViewRow3;

    vec4 uProjRow0;
    vec4 uProjRow1;
    vec4 uProjRow2;
    vec4 uProjRow3;

    vec4 uModelRow0;
    vec4 uModelRow1;
    vec4 uModelRow2;
    vec4 uModelRow3;

    vec4 uViewportParams;
    vec4 uTimeParams;
    vec4 uRenderParams; // x=uScale, y=uPixelRatio, z=uScreenHeight, w=uDensityComp
};

vec4 mulRows(vec4 v, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
{
    return vec4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vDist;
layout(location = 3) out float vOpacity;
layout(location = 4) out float vScaleFactor;
layout(location = 5) out float vIsRing;

float smoothStep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float hash(float n)
{
    uint x = floatBitsToUint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

float fastSin(float x)
{
    x = mod(x, 6.28318530718);
    x = x > 3.14159265359 ? x - 6.28318530718 : x;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * (0.00833333 - x2 * 0.0001984)));
}

void main()
{
    vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2( 1.0, -1.0)
    );

    vec2 corner = corners[gl_VertexIndex % 6];

    ParticleData p = particles[gl_InstanceIndex];

    float uScale        = uRenderParams.x;
    float uPixelRatio   = uRenderParams.y;
    float uScreenHeight = uRenderParams.z;

    vec4 col;
    col.r = float( p.color        & 0xFFu) / 255.0;
    col.g = float((p.color >>  8) & 0xFFu) / 255.0;
    col.b = float((p.color >> 16) & 0xFFu) / 255.0;
    col.a = float((p.color >> 24) & 0xFFu) / 255.0;

    vec4 worldPos = mulRows(vec4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    vec4 mvPosition  = mulRows(worldPos,                   uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);

    float dist = -mvPosition.z;

    if (mvPosition.z >= -0.001)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUV    = vec2(0.0);
        vColor = vec3(0.0);
        vDist = 0.0;
        vOpacity = 0.0;
        vScaleFactor = 0.0;
        vIsRing = 0.0;
        return;
    }

    float chaosThreshold = 25.0;
    float chaosIntensity = smoothStep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    vec3 noiseVec = vec3(0.0);
    if (chaosIntensity > 0.001)
    {
        float highFreqTime = uTimeParams.x * 40.0;
        vec3 posScaled = p.pos.xyz * 10.0;
        float hashX = hash(p.pos.y * 43758.5) * 0.5;
        float hashY = hash(p.pos.x * 43758.5) * 0.5;
        float hashZ = hash(p.pos.z * 43758.5) * 0.5;
        noiseVec = vec3(
            fastSin(highFreqTime + posScaled.x) * hashX,
            fastSin(highFreqTime + posScaled.y + 1.5708) * hashY,
            fastSin(highFreqTime * 0.5) * hashZ
        ) * 3.0;
    }
    mvPosition.xyz = mix(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    vec4 clipPos  = mulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);

    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = p.pos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;
    float nearMask = (dist <= 50.0) ? 1.0 : 0.0;
    float ringFactor = mix(mix(1.0, 0.8, nearMask), 1.0, p.isRing);
    pointSize *= ringFactor * pow(max(uPixelRatio, 0.0001), 0.8);
    float px = clamp(pointSize, 0.0, 300.0 * screenScale);

    vec2 ndcCenter = clipPos.xy / clipPos.w;
    vec2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    vec2 ndcPos    = ndcCenter + ndcOffset;

    gl_Position = vec4(ndcPos * clipPos.w, clipPos.z, clipPos.w);

    vUV = corner * 0.5 + 0.5;
    vColor = col.rgb;
    vDist  = dist;
    vOpacity = col.a;
    vScaleFactor = uScale;
    vIsRing = p.isRing;
}
)";

    static constexpr char kGlslPS[] = R"(
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vDist;
layout(location = 3) in float vOpacity;
layout(location = 4) in float vScaleFactor;
layout(location = 5) in float vIsRing;

layout(set=0, binding=1, std140) uniform ParticleConstants
{
    vec4 uViewRow0;
    vec4 uViewRow1;
    vec4 uViewRow2;
    vec4 uViewRow3;

    vec4 uProjRow0;
    vec4 uProjRow1;
    vec4 uProjRow2;
    vec4 uProjRow3;

    vec4 uModelRow0;
    vec4 uModelRow1;
    vec4 uModelRow2;
    vec4 uModelRow3;

    vec4 uViewportParams;
    vec4 uTimeParams;
    vec4 uRenderParams; // w=uDensityComp
};

layout(location = 0) out vec4 oColor;

float smoothStep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main()
{
    vec2 c = 2.0 * vUV - 1.0;
    float rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float glow = smoothStep(1.0, 0.4, rr);

    float t = clamp((vScaleFactor - 0.15) * 0.4255, 0.0, 1.0);
    float tSmooth = smoothStep(0.1, 0.9, t);

    vec3 baseColor = mix(vec3(0.35, 0.22, 0.05), vColor, tSmooth);
    vec3 finalColor = baseColor * (0.2 + t);

    float closeMix = smoothStep(40.0, 0.0, vDist);
    vec3 closeRingColor = finalColor + vec3(0.15, 0.12, 0.1) * closeMix;
    vec3 closeBodyColor = mix(finalColor, pow(vColor, vec3(1.4)) * 1.5, closeMix * 0.8);
    finalColor = mix(closeBodyColor, closeRingColor, vIsRing);

    float depthAlpha = smoothStep(0.0, 10.0, vDist);
    float densityComp = uRenderParams.w;
    float finalAlpha = glow * vOpacity * (0.25 + 0.45 * smoothStep(0.0, 0.5, t)) * depthAlpha * densityComp;

    oColor = vec4(finalColor, finalAlpha);
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslVS, kGlslPS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslVS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

MeshShaderSources GetSaturnParticleMeshShaderSources(Backend backend) {
    // Mesh Shader 版粒子渲染：每个 workgroup 处理多个粒子，直接生成顶点和图元
    // 相比 Vertex Shader + Instancing，减少了 VS 调用次数和顶点属性获取开销
    //
    // 每个粒子生成 4 个顶点 + 2 个三角形（6 个索引）
    // 线程组大小：32 粒子/组，最大输出 128 顶点 + 64 三角形

    static constexpr char kHlslMS[] = R"(
#define GROUP_SIZE 32
#define VERTS_PER_PARTICLE 4
#define PRIMS_PER_PARTICLE 2
#define MAX_VERTS (GROUP_SIZE * VERTS_PER_PARTICLE)  // 128
#define MAX_PRIMS (GROUP_SIZE * PRIMS_PER_PARTICLE)  // 64

struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

StructuredBuffer<ParticleData> g_Particles;

cbuffer ParticleConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    float4 uViewportParams;
    float4 uTimeParams;
    float4 uRenderParams;

    uint   uParticleCount;
    uint3  _pad;
};

struct VertexOutput
{
    float4 Pos         : SV_POSITION;
    float2 UV          : TEXCOORD0;
    float3 vColor      : TEXCOORD1;
    float  vDist       : TEXCOORD2;
    float  vOpacity    : TEXCOORD3;
    float  vScaleFactor: TEXCOORD4;
    float  vIsRing     : TEXCOORD5;
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float4 UnpackRGBA8(uint c)
{
    return float4(
        float((c      ) & 0xFF) / 255.0,
        float((c >>  8) & 0xFF) / 255.0,
        float((c >> 16) & 0xFF) / 255.0,
        float((c >> 24) & 0xFF) / 255.0
    );
}

float SmoothStep(float edge0, float edge1, float x)
{
    float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

float Hash(float n)
{
    uint x = asuint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

[outputtopology("triangle")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices VertexOutput verts[MAX_VERTS],
    out indices uint3 tris[MAX_PRIMS])
{
    uint particleIdx = gid * GROUP_SIZE + gtid;

    // 计算这个组实际要处理的粒子数
    uint groupStart = gid * GROUP_SIZE;
    uint groupEnd = min(groupStart + GROUP_SIZE, uParticleCount);
    uint particlesInGroup = groupEnd - groupStart;

    // 设置输出数量（所有线程必须调用相同的值）
    SetMeshOutputCounts(particlesInGroup * VERTS_PER_PARTICLE, particlesInGroup * PRIMS_PER_PARTICLE);

    // 超出粒子数量的线程不需要生成输出
    if (gtid >= particlesInGroup)
        return;

    ParticleData p = g_Particles[particleIdx];
    float4 col = UnpackRGBA8(p.color);

    float uScale        = uRenderParams.x;
    float uPixelRatio   = uRenderParams.y;
    float uScreenHeight = uRenderParams.z;

    float4 worldPos   = MulRows(float4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 mvPosition = MulRows(worldPos, uViewRow0, uViewRow1, uViewRow2, uViewRow3);

    float dist = -mvPosition.z;

    // 剔除相机后方的粒子
    bool valid = (mvPosition.z < -0.001);

    float3 vColor = float3(0, 0, 0);
    float vOpacity = 0.0;
    float vScaleFactor = 0.0;
    float4 clipPos = float4(2, 2, 2, 1);
    float halfSize = 0.0;

    if (valid)
    {
        // 简化的点大小计算
        float baseSize = p.pos.w * 350.0;
        float distFactor = 1.0 / dist;
        float screenScale = uScreenHeight / 1080.0;
        float ringFactor = (p.isRing > 0.5) ? 0.85 : 1.0;
        float pixelFactor = pow(abs(uPixelRatio), 0.8);

        float pointSize = baseSize * distFactor * 0.55 * screenScale * ringFactor * pixelFactor;
        pointSize = clamp(pointSize, 0.5, 100.0);
        halfSize = pointSize * 0.5;

        vScaleFactor = pointSize;
        vOpacity = col.a;
        vColor = col.rgb;

        clipPos = MulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);
    }

    // 四个角的偏移（屏幕空间）
    float2 corners[4] = {
        float2(-1, -1),
        float2(-1,  1),
        float2( 1,  1),
        float2( 1, -1)
    };

    // 组内索引直接使用线程 ID
    uint baseVert = gtid * VERTS_PER_PARTICLE;
    uint basePrim = gtid * PRIMS_PER_PARTICLE;

    // 生成 4 个顶点
    for (uint i = 0; i < VERTS_PER_PARTICLE; i++)
    {
        VertexOutput v;
        v.UV = corners[i] * 0.5 + 0.5;
        v.vColor = vColor;
        v.vDist = dist;
        v.vOpacity = valid ? vOpacity : 0.0;
        v.vScaleFactor = vScaleFactor;
        v.vIsRing = p.isRing;

        if (valid)
        {
            float2 offset = corners[i] * halfSize;
            v.Pos = clipPos;
            v.Pos.xy += offset * uViewportParams.xy * clipPos.w;
        }
        else
        {
            v.Pos = float4(2, 2, 2, 1);
        }

        verts[baseVert + i] = v;
    }

    // 生成 2 个三角形
    tris[basePrim + 0] = uint3(baseVert + 0, baseVert + 1, baseVert + 2);
    tris[basePrim + 1] = uint3(baseVert + 0, baseVert + 2, baseVert + 3);
}
)";

    // Mesh Shader 使用与 Vertex Shader 类似的 Pixel Shader，但 vScaleFactor 语义不同
    static constexpr char kHlslPS[] = R"(
struct PSIn
{
    float4 Pos         : SV_POSITION;
    float2 UV          : TEXCOORD0;
    float3 vColor      : TEXCOORD1;
    float  vDist       : TEXCOORD2;
    float  vOpacity    : TEXCOORD3;
    float  vScaleFactor: TEXCOORD4;
    float  vIsRing     : TEXCOORD5;
};

cbuffer ParticleConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;
    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;
    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;
    float4 uViewportParams;
    float4 uTimeParams;
    float4 uRenderParams;
};

float SmoothStep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = saturate(t);
    return t * t * (3.0 - 2.0 * t);
}

float4 main(PSIn i) : SV_Target
{
    float2 c = 2.0 * i.UV - 1.0;
    float  rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float glow = SmoothStep(1.0, 0.4, rr);

    // vScaleFactor 在 Mesh Shader 中是 pointSize，需要转换为与 Vertex Shader 兼容的 t
    float t = clamp((i.vScaleFactor - 0.5) / 40.0, 0.0, 1.0);
    float tSmooth = SmoothStep(0.1, 0.9, t);

    float3 baseColor  = lerp(float3(0.35, 0.22, 0.05), i.vColor, tSmooth);
    float3 finalColor = baseColor * (0.2 + t);

    float closeMix = SmoothStep(40.0, 0.0, i.vDist);
    float3 closeRingColor = finalColor + float3(0.15, 0.12, 0.1) * closeMix;
    float3 closeBodyColor = lerp(finalColor, pow(i.vColor, float3(1.4, 1.4, 1.4)) * 1.5, closeMix * 0.8);
    finalColor = lerp(closeBodyColor, closeRingColor, i.vIsRing);

    // 与 Vertex Pulling PS 保持一致：近处 alpha 低，远处 alpha 高
    float depthAlpha = SmoothStep(0.0, 10.0, i.vDist);
    float densityComp = uRenderParams.w;
    float finalAlpha = glow * i.vOpacity * (0.25 + 0.45 * SmoothStep(0.0, 0.5, t)) * depthAlpha * densityComp;

    return float4(finalColor, finalAlpha);
}
)";

    // 注意：Vulkan Mesh Shader 需要 VK_EXT_mesh_shader 扩展和不同的 GLSL 语法
    // 暂时只支持 D3D12，Vulkan 返回空
    if (backend == Backend::Vulkan) {
        return {nullptr, nullptr, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    if (backend == Backend::D3D11) {
        // D3D11 不支持 Mesh Shader
        return {nullptr, nullptr, SHADER_SOURCE_LANGUAGE_HLSL};
    }
    return {kHlslMS, kHlslPS, SHADER_SOURCE_LANGUAGE_HLSL};
}

} // namespace ParticleSaturn::Render
