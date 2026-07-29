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
    uint   isRing;
    uint   pad;
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
    uint isRing = 0u;

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
        isRing = 0u;
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
        isRing = 1u;
    }

    ParticleData p;
    p.pos = pos;
    p.color = PackRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.pad = 0u;
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
    uint  isRing;
    uint  pad;
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
uint pcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
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
    uint isRing = 0u;

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
        isRing = 0u;
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
        isRing = 1u;
    }

    ParticleData p;
    p.pos = pos;
    p.color = packRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.pad = 0u;
    particlesOut[id] = p;
}
)";

    if (backend == Backend::Vulkan) {
        return {kGlslCS, SHADER_SOURCE_LANGUAGE_GLSL};
    }
    return {kHlslCS, SHADER_SOURCE_LANGUAGE_HLSL};
}

} // namespace ParticleSaturn::Render
