#include "DiligentBackend.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <random>
#include <vector>

#include "../DebugLog.h"
#include "EngineFactoryD3D12.h"
#include "EngineFactoryVk.h"
#include "GraphicsTypes.h"
#include "ImGuiDiligent.h"
#include "InputLayout.h"
#include "NativeWindow.h"
#include "Sampler.h"
#include "imgui.h"
#include "md3/MD3.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ParticleSaturn::Render {

using namespace Diligent;

// 构造函数和析构函数
DiligentBackend::DiligentBackend() = default;

DiligentBackend::~DiligentBackend() {
    Shutdown();
}

namespace {

static constexpr TEXTURE_FORMAT kOffscreenColorFormat = TEX_FORMAT_R11G11B10_FLOAT;

static constexpr uint32_t kStarDensityBaseWidth  = 1920;
static constexpr uint32_t kStarDensityBaseHeight = 1080;
static constexpr uint32_t kStarDensityBaseCount  = 50000;
static constexpr uint32_t kStarDensityMinCount   = 5000;
static constexpr uint32_t kStarDensityMaxCount   = 500000;

uint32_t ComputeStarCountForResolution(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return kStarDensityBaseCount;
    }
    const double baseArea = static_cast<double>(kStarDensityBaseWidth) * static_cast<double>(kStarDensityBaseHeight);
    const double area     = static_cast<double>(width) * static_cast<double>(height);
    const double scale    = (baseArea > 0.0) ? (area / baseArea) : 1.0;

    double countD = static_cast<double>(kStarDensityBaseCount) * scale;
    if (countD < static_cast<double>(kStarDensityMinCount)) {
        countD = static_cast<double>(kStarDensityMinCount);
    }
    if (countD > static_cast<double>(kStarDensityMaxCount)) {
        countD = static_cast<double>(kStarDensityMaxCount);
    }

    // 四舍五入，保证 1920x1080 下刚好是 50000。
    return static_cast<uint32_t>(countD + 0.5);
}

struct ShaderSources {
    const char*            Vertex   = nullptr;
    const char*            Fragment = nullptr;
    SHADER_SOURCE_LANGUAGE Language = SHADER_SOURCE_LANGUAGE_DEFAULT;
};

struct ComputeShaderSource {
    const char*            Source   = nullptr;
    SHADER_SOURCE_LANGUAGE Language = SHADER_SOURCE_LANGUAGE_DEFAULT;
};

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
    float3 _pad;
};

float3 ToneMap(float3 hdr)
{
    return hdr / (hdr + float3(1.0, 1.0, 1.0));
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

    oColor = vec4(col, 1.0);
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

RefCntAutoPtr<IShader> CreateShaderFromSource(IRenderDevice* device, const char* name, SHADER_TYPE type,
                                              const char* source, SHADER_SOURCE_LANGUAGE language) {
    ShaderCreateInfo shaderCI{};
    shaderCI.Desc.Name       = name;
    shaderCI.Desc.ShaderType = type;
    shaderCI.SourceLanguage  = language;
    shaderCI.EntryPoint      = "main";
    shaderCI.Source          = source;

    RefCntAutoPtr<IShader> shader;
    device->CreateShader(shaderCI, &shader);
    return shader;
}

struct StarInstance {
    float Pos[3]   = {0.0f, 0.0f, 0.0f};
    float Color[3] = {1.0f, 1.0f, 1.0f};
    float Size     = 1.0f;
    float Seed     = 0.0f;
};

struct SaturnParticle {
    float    Pos[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyz + scale
    uint32_t Color  = 0xFFFFFFFFu;              // RGBA8 packed
    float    Speed  = 0.0f;
    float    IsRing = 0.0f;
    float    Pad    = 0.0f;
};

struct Mat4Rows {
    float Row[4][4] = {
        {1, 0, 0, 0},
        {0, 1, 0, 0},
        {0, 0, 1, 0},
        {0, 0, 0, 1},
    };
};

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Normalize(Vec3 v) {
    const float len2 = Dot(v, v);
    if (len2 <= 0.0f) {
        return {0, 0, 0};
    }
    const float inv = 1.0f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

Mat4Rows LookAtRH(Vec3 eye, Vec3 center, Vec3 up) {
    // 右手系 lookAt（与 OpenGL 风格一致）：相机朝向 -Z。
    const Vec3 f = Normalize(Sub(center, eye));
    const Vec3 s = Normalize(Cross(f, up));
    const Vec3 u = Cross(s, f);

    Mat4Rows m{};
    m.Row[0][0] = s.x;
    m.Row[0][1] = s.y;
    m.Row[0][2] = s.z;
    m.Row[0][3] = -Dot(s, eye);

    m.Row[1][0] = u.x;
    m.Row[1][1] = u.y;
    m.Row[1][2] = u.z;
    m.Row[1][3] = -Dot(u, eye);

    m.Row[2][0] = -f.x;
    m.Row[2][1] = -f.y;
    m.Row[2][2] = -f.z;
    m.Row[2][3] = Dot(f, eye);

    m.Row[3][0] = 0.0f;
    m.Row[3][1] = 0.0f;
    m.Row[3][2] = 0.0f;
    m.Row[3][3] = 1.0f;
    return m;
}

Mat4Rows PerspectiveRH_OpenGL(float fovyRad, float aspect, float zNear, float zFar) {
    // OpenGL 风格的 RH 投影矩阵（clip.w = -view.z），主要用于正确的 x/y 投影与深度相关点大小。
    const float f = 1.0f / std::tan(fovyRad * 0.5f);
    Mat4Rows    m{};
    m.Row[0][0] = f / aspect;
    m.Row[0][1] = 0.0f;
    m.Row[0][2] = 0.0f;
    m.Row[0][3] = 0.0f;

    m.Row[1][0] = 0.0f;
    m.Row[1][1] = f;
    m.Row[1][2] = 0.0f;
    m.Row[1][3] = 0.0f;

    m.Row[2][0] = 0.0f;
    m.Row[2][1] = 0.0f;
    m.Row[2][2] = (zFar + zNear) / (zNear - zFar);
    m.Row[2][3] = (2.0f * zFar * zNear) / (zNear - zFar);

    m.Row[3][0] = 0.0f;
    m.Row[3][1] = 0.0f;
    m.Row[3][2] = -1.0f;
    m.Row[3][3] = 0.0f;

    return m;
}

Mat4Rows RotationY(float a) {
    const float s = std::sin(a);
    const float c = std::cos(a);

    Mat4Rows m{};
    m.Row[0][0] = c;
    m.Row[0][1] = 0.0f;
    m.Row[0][2] = s;
    m.Row[0][3] = 0.0f;

    m.Row[1][0] = 0.0f;
    m.Row[1][1] = 1.0f;
    m.Row[1][2] = 0.0f;
    m.Row[1][3] = 0.0f;

    m.Row[2][0] = -s;
    m.Row[2][1] = 0.0f;
    m.Row[2][2] = c;
    m.Row[2][3] = 0.0f;

    m.Row[3][0] = 0.0f;
    m.Row[3][1] = 0.0f;
    m.Row[3][2] = 0.0f;
    m.Row[3][3] = 1.0f;
    return m;
}

Mat4Rows RotationX(float a) {
    const float s = std::sin(a);
    const float c = std::cos(a);

    Mat4Rows m{};
    m.Row[0][0] = 1.0f;
    m.Row[0][1] = 0.0f;
    m.Row[0][2] = 0.0f;
    m.Row[0][3] = 0.0f;

    m.Row[1][0] = 0.0f;
    m.Row[1][1] = c;
    m.Row[1][2] = -s;
    m.Row[1][3] = 0.0f;

    m.Row[2][0] = 0.0f;
    m.Row[2][1] = s;
    m.Row[2][2] = c;
    m.Row[2][3] = 0.0f;

    m.Row[3][0] = 0.0f;
    m.Row[3][1] = 0.0f;
    m.Row[3][2] = 0.0f;
    m.Row[3][3] = 1.0f;
    return m;
}

Mat4Rows RotationZ(float a) {
    const float s = std::sin(a);
    const float c = std::cos(a);

    Mat4Rows m{};
    m.Row[0][0] = c;
    m.Row[0][1] = -s;
    m.Row[0][2] = 0.0f;
    m.Row[0][3] = 0.0f;

    m.Row[1][0] = s;
    m.Row[1][1] = c;
    m.Row[1][2] = 0.0f;
    m.Row[1][3] = 0.0f;

    m.Row[2][0] = 0.0f;
    m.Row[2][1] = 0.0f;
    m.Row[2][2] = 1.0f;
    m.Row[2][3] = 0.0f;

    m.Row[3][0] = 0.0f;
    m.Row[3][1] = 0.0f;
    m.Row[3][2] = 0.0f;
    m.Row[3][3] = 1.0f;
    return m;
}

Mat4Rows Mul(Mat4Rows a, Mat4Rows b) {
    Mat4Rows r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = 0.0f;
            for (int k = 0; k < 4; ++k) {
                v += a.Row[i][k] * b.Row[k][j];
            }
            r.Row[i][j] = v;
        }
    }
    return r;
}

struct StarConstants {
    float ViewRow0[4] = {1, 0, 0, 0};
    float ViewRow1[4] = {0, 1, 0, 0};
    float ViewRow2[4] = {0, 0, 1, 0};
    float ViewRow3[4] = {0, 0, 0, 1};

    float ProjRow0[4] = {1, 0, 0, 0};
    float ProjRow1[4] = {0, 1, 0, 0};
    float ProjRow2[4] = {0, 0, 1, 0};
    float ProjRow3[4] = {0, 0, 0, 1};

    float ModelRow0[4] = {1, 0, 0, 0};
    float ModelRow1[4] = {0, 1, 0, 0};
    float ModelRow2[4] = {0, 0, 1, 0};
    float ModelRow3[4] = {0, 0, 0, 1};

    // x = 2/width, y = 2/height
    float ViewportParams[4] = {0, 0, 0, 0};

    float TimeParams[4] = {0, 0, 0, 0};

    // 复用给粒子：x=uScale, y=uPixelRatio, z=uScreenHeight, w=uDensityComp
    float RenderParams[4] = {1, 1, 1080, 1};
};

struct ParticleComputeConstants {
    float    Dt            = 0.0f;
    float    HandScale     = 1.0f;
    float    HandHas       = 0.0f;
    uint32_t ParticleCount = 0u;
};

constexpr float HexToFloat(uint32_t v) {
    return static_cast<float>(v) / 255.0f;
}

void HexToRGB(uint32_t rgb, float outColor[3]) {
    outColor[0] = HexToFloat((rgb >> 16) & 0xFF);
    outColor[1] = HexToFloat((rgb >> 8) & 0xFF);
    outColor[2] = HexToFloat((rgb >> 0) & 0xFF);
}

float Random01(uint32_t& state) {
    state           = state * 747796405u + 2891336453u;
    uint32_t result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result          = (result >> 22u) ^ result;
    return static_cast<float>(result) / 4294967295.0f;
}

uint32_t PackRGBA8(float r, float g, float b, float a) {
    auto clamp01 = [](float x) {
        if (x < 0.0f) {
            return 0.0f;
        }
        if (x > 1.0f) {
            return 1.0f;
        }
        return x;
    };
    const uint32_t ur = static_cast<uint32_t>(clamp01(r) * 255.0f);
    const uint32_t ug = static_cast<uint32_t>(clamp01(g) * 255.0f);
    const uint32_t ub = static_cast<uint32_t>(clamp01(b) * 255.0f);
    const uint32_t ua = static_cast<uint32_t>(clamp01(a) * 255.0f);
    return (ur) | (ug << 8u) | (ub << 16u) | (ua << 24u);
}

Vec3 HexToRGB(uint32_t hex) {
    return {HexToFloat((hex >> 16) & 0xFF), HexToFloat((hex >> 8) & 0xFF), HexToFloat(hex & 0xFF)};
}

Vec3 Mix(Vec3 a, Vec3 b, float t) {
    return {a.x * (1.0f - t) + b.x * t, a.y * (1.0f - t) + b.y * t, a.z * (1.0f - t) + b.z * t};
}

std::vector<SaturnParticle> InitSaturnParticlesCPU(uint32_t maxParticles, uint32_t seed) {
    std::vector<SaturnParticle> out;
    out.resize(maxParticles);

    constexpr float R = 18.0f;

    for (uint32_t id = 0; id < maxParticles; ++id) {
        uint32_t rngState = id * 1973u + seed * 9277u + 26699u;

        const float typeRnd = Random01(rngState);

        SaturnParticle p{};
        float          alpha = 1.0f;
        Vec3           colRGB{1, 1, 1};
        float          speed  = 0.0f;
        float          isRing = 0.0f;

        if (typeRnd < 0.25f) {
            // --- 土星本体粒子 ---
            const float th = 6.28318f * Random01(rngState);
            const float ph = std::acos(2.0f * Random01(rngState) - 1.0f);

            p.Pos[0] = R * std::sin(ph) * std::cos(th);
            p.Pos[1] = R * std::cos(ph) * 0.9f;
            p.Pos[2] = R * std::sin(ph) * std::sin(th);

            const float lat = (p.Pos[1] / 0.9f / R + 1.0f) * 0.5f;
            const int   idxInt =
                static_cast<int>(lat * 4.0f + std::cos(lat * 40.0f) * 0.8f + std::cos(lat * 15.0f) * 0.4f);
            int ci = idxInt - (idxInt / 4) * 4;
            if (ci < 0) {
                ci = 0;
            }

            const Vec3 cols[4] = {HexToRGB(0xE3DAC5), HexToRGB(0xC9A070), HexToRGB(0xE3DAC5), HexToRGB(0xB08D55)};
            colRGB             = cols[ci];

            p.Pos[3] = 1.0f + Random01(rngState) * 0.8f;
            alpha    = 0.8f;
            speed    = 0.0f;
            isRing   = 0.0f;
        } else {
            // --- 土星环粒子 ---
            const float z   = Random01(rngState);
            float       rad = 0.0f;
            Vec3        c{1, 1, 1};
            float       s = 1.0f;
            float       o = 1.0f;

            if (z < 0.15f) {
                rad = R * (1.235f + Random01(rngState) * 0.29f);
                c   = HexToRGB(0x2A2520);
                s   = 0.5f;
                o   = 0.3f;
            } else if (z < 0.65f) {
                const float t = Random01(rngState);
                rad           = R * (1.525f + t * 0.425f);
                c             = Mix(HexToRGB(0xCDBFA0), HexToRGB(0xDCCBBA), t);
                s             = 0.8f + Random01(rngState) * 0.6f;
                o             = 0.85f;
                if (std::sin(rad * 2.0f) > 0.8f) {
                    o *= 1.2f;
                }
            } else if (z < 0.69f) {
                rad = R * (1.95f + Random01(rngState) * 0.075f);
                c   = HexToRGB(0x050505);
                s   = 0.3f;
                o   = 0.1f;
            } else if (z < 0.99f) {
                rad = R * (2.025f + Random01(rngState) * 0.245f);
                c   = HexToRGB(0x989085);
                s   = 0.7f;
                o   = 0.6f;
                if (rad > R * 2.2f && rad < R * 2.21f) {
                    o = 0.1f;
                }
            } else {
                rad = R * (2.32f + Random01(rngState) * 0.02f);
                c   = HexToRGB(0xAFAFA0);
                s   = 1.0f;
                o   = 0.7f;
            }

            const float th = Random01(rngState) * 6.28318f;
            p.Pos[0]       = rad * std::cos(th);
            p.Pos[2]       = rad * std::sin(th);

            const float heightRange = (rad > R * 2.3f) ? 0.4f : 0.15f;
            p.Pos[1]                = (Random01(rngState) - 0.5f) * heightRange;

            colRGB   = c;
            p.Pos[3] = s;
            alpha    = o;
            speed    = 8.0f / std::sqrt(rad);
            isRing   = 1.0f;
        }

        p.Color  = PackRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
        p.Speed  = speed;
        p.IsRing = isRing;
        out[id]  = p;
    }

    return out;
}

// ============================================
// 七段数码管着色器（用于 FPS 显示）
// ============================================

// 数字定义（与 OpenGL 版一致）：每个数字 7 段，1=点亮
// 段编号: 0=top, 1=top-right, 2=bottom-right, 3=bottom, 4=bottom-left, 5=top-left, 6=middle
constexpr int kDigits[10][7] = {
    {1, 1, 1, 1, 1, 1, 0}, // 0
    {0, 1, 1, 0, 0, 0, 0}, // 1
    {1, 1, 0, 1, 1, 0, 1}, // 2
    {1, 1, 1, 1, 0, 0, 1}, // 3
    {0, 1, 1, 0, 0, 1, 1}, // 4
    {1, 0, 1, 1, 0, 1, 1}, // 5
    {1, 0, 1, 1, 1, 1, 1}, // 6
    {1, 1, 1, 0, 0, 0, 0}, // 7
    {1, 1, 1, 1, 1, 1, 1}, // 8
    {1, 1, 1, 1, 0, 1, 1}, // 9
};

static constexpr char kSevenSegHlslVS[] = R"(
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
)";

static constexpr char kSevenSegHlslPS[] = R"(
struct PSIn
{
    float4 Pos : SV_POSITION;
    float3 Col : COLOR;
};

float4 main(PSIn i) : SV_Target
{
    return float4(i.Col, 1.0);
}
)";

static constexpr char kSevenSegGlslVS[] = R"(
layout(std140) uniform SevenSegCB
{
    mat4 Projection;
    vec4 Transform; // x, y, scaleX, scaleY
    vec4 Color;     // rgb + pad
};

layout(location = 0) in vec2 inPos;
layout(location = 0) out vec3 vColor;

void main()
{
    vec2 worldPos = inPos * Transform.zw + Transform.xy;
    gl_Position = Projection * vec4(worldPos, 0.0, 1.0);
    vColor = Color.rgb;
}
)";

static constexpr char kSevenSegGlslPS[] = R"(
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(vColor, 1.0);
}
)";

} // namespace

bool DiligentBackend::Init(Backend backend, HWND hwnd, SurfaceSize initialSize, AppState* state) {
    backend_  = backend;
    appState_ = state;
    SetLastError(nullptr);

    if (hwnd == nullptr || initialSize.Width == 0 || initialSize.Height == 0) {
        SetLastError(L"Init 参数无效（HWND 或尺寸为 0）。");
        return false;
    }

    device_.Release();
    immediateContext_.Release();
    swapChain_.Release();

    const NativeWindow window{reinterpret_cast<void*>(hwnd)};

    SwapChainDesc scDesc{};
    scDesc.Width             = initialSize.Width;
    scDesc.Height            = initialSize.Height;
    scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    scDesc.BufferCount       = 3; // 三缓冲，避免 D3D12 帧等待超时
    // 阶段 1：引入深度缓冲（即便当下的全屏四边形不依赖深度测试，先把链路补齐）。
    scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;

    if (backend == Backend::D3D12) {
        auto* factory = GetEngineFactoryD3D12();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryD3D12() 返回空。");
            return false;
        }

        EngineD3D12CreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif
        factory->CreateDeviceAndContextsD3D12(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"D3D12 设备或上下文创建失败。");
            return false;
        }

        FullScreenModeDesc fsDesc{};
        factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
    } else {
        auto* factory = GetEngineFactoryVk();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryVk() 返回空。");
            return false;
        }

        EngineVkCreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif
        factory->CreateDeviceAndContextsVk(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"Vulkan 设备或上下文创建失败。");
            return false;
        }

        factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);
    }

    if (swapChain_ == nullptr) {
        SetLastError(L"SwapChain 创建失败。");
        return false;
    }

    // 注意：Win32 的 WM_SIZE/ClientRect 尺寸在某些 DPI/缩放配置下可能与 SwapChain 实际尺寸不完全一致。
    // 后续渲染/点精灵的像素尺寸换算依赖“真实 RT 尺寸”，这里优先以 SwapChainDesc 为准。
    const auto& scFinalDesc = swapChain_->GetDesc();
    surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
    startTime_              = std::chrono::steady_clock::now();
    lastAnimTime_           = std::chrono::steady_clock::time_point{};
    animAutoTime_           = 0.0f;
    animScale_              = 1.0f;
    animRotX_               = 0.4f;
    animRotY_               = 0.0f;

    if (!CreateFullscreenQuadPSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateFullscreenQuadPSO() 失败。");
        }
        return false;
    }

    if (!CreateOffscreenRenderTarget(surfaceSize_)) {
        if (lastError_.empty()) {
            SetLastError(L"CreateOffscreenRenderTarget() 失败。");
        }
        return false;
    }
    UpdateFullscreenQuadBindings();

    // 阶段 2：星空（先用 2D NDC 点列表验证 point 渲染 + 闪烁 + 混合链路）。
    // 星空密度与分辨率无关：以 OpenGL 版在 1920x1080 下的密度（5 万）为基准，按像素面积等比缩放星星数量。
    const uint32_t desiredStarCount = ComputeStarCountForResolution(surfaceSize_.Width, surfaceSize_.Height);
    if (!CreateStarfieldBuffers(desiredStarCount)) {
        if (lastError_.empty()) {
            SetLastError(L"CreateStarfieldBuffers() 失败。");
        }
        return false;
    }
    if (!CreateStarfieldPSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateStarfieldPSO() 失败。");
        }
        return false;
    }

    // 阶段 3（第 1 步）：粒子数据通路（先 CPU 复刻初始化，后续再接 compute 三缓冲轮转）。
    // 复刻 OpenGL 旧版默认粒子规模：120 万（视觉遮蔽/密度/“不透光感”强相关）。
    if (!CreateParticleBuffers(1200000)) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleBuffers() 失败。");
        }
        return false;
    }
    if (!CreateParticlePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticlePSO() 失败。");
        }
        return false;
    }
    if (!CreateParticleComputePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleComputePSO() 失败。");
        }
        return false;
    }

    // 阶段 5：七段数码管 FPS 显示
    if (!CreateSevenSegmentPSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateSevenSegmentPSO() 失败。");
        }
        return false;
    }
    if (!CreateSevenSegmentBuffers()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateSevenSegmentBuffers() 失败。");
        }
        return false;
    }

    // 阶段 5：ImGui 初始化
    hwnd_  = hwnd;
    imgui_ = std::make_unique<UI::ImGuiDiligent>();
    if (!imgui_->Init(hwnd, backend, device_, swapChain_)) {
        if (lastError_.empty()) {
            SetLastError(L"ImGui 初始化失败。");
        }
        return false;
    }

    // 阶段 6：MD3 UI 系统初始化
    MD3::Init(device_, immediateContext_, 1.0f);
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));
    MD3::ApplyImGuiStyle();

    return true;
}

void DiligentBackend::Shutdown() {
    // 先关闭 MD3（在 ImGui 之前）
    MD3::Shutdown();

    // 关闭 ImGui（在释放设备前）
    if (imgui_) {
        imgui_->Shutdown();
        imgui_.reset();
    }

    offscreenRTV_.Release();
    offscreenSRV_.Release();
    offscreenColor_.Release();
    fullscreenQuadSRB_.Release();
    fullscreenQuadPSO_.Release();

    starSRB_.Release();
    starVB_.Release();
    starConstants_.Release();
    starPSO_.Release();

    particleSRB_.Release();
    particleConstants_.Release();
    particlePSO_.Release();
    particleIndirectArgs_.Release();

    particleComputeSRB_.Release();
    particleComputeConstants_.Release();
    particleComputePSO_.Release();
    for (auto& v : particleUAVs_) {
        v.Release();
    }
    for (auto& v : particleSRVs_) {
        v.Release();
    }
    for (auto& b : particleBuffers_) {
        b.Release();
    }
    particleCount_ = 0;

    swapChain_.Release();
    immediateContext_.Release();
    device_.Release();
}

void DiligentBackend::Resize(SurfaceSize newSize) {
    if (swapChain_ == nullptr) {
        return;
    }
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }

    const auto& curDesc = swapChain_->GetDesc();
    if (curDesc.Width == newSize.Width && curDesc.Height == newSize.Height) {
        return;
    }

    swapChain_->Resize(newSize.Width, newSize.Height);
    const auto& newDesc = swapChain_->GetDesc();
    surfaceSize_        = {newDesc.Width, newDesc.Height};

    // SwapChain Resize 只影响后备缓冲/深度缓冲；离屏 RT 需要手动重建。
    CreateOffscreenRenderTarget(surfaceSize_);
    UpdateFullscreenQuadBindings();

    // 星空密度与分辨率无关：按面积缩放星星数量（以 1920x1080 的 5 万为基准）。
    const uint32_t desiredStarCount = ComputeStarCountForResolution(surfaceSize_.Width, surfaceSize_.Height);
    if (desiredStarCount != starCount_ && starPSO_ != nullptr) {
        CreateStarfieldBuffers(desiredStarCount);
    }

    // 更新 MD3 屏幕尺寸
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));
}

bool DiligentBackend::CreateFullscreenQuadPSO() {
    if (device_ == nullptr || swapChain_ == nullptr) {
        return false;
    }

    const auto sources = GetFullscreenQuadShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "FullscreenQuad VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language);
    const auto ps =
        CreateShaderFromSource(device_, "FullscreenQuad PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "FullscreenQuad PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    const auto& scDesc = swapChain_->GetDesc();

    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = scDesc.ColorBufferFormat;
    // 当前阶段的离屏 RT 不带深度；PSO 也不绑定 DSV。
    psoCI.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;

    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // 离屏合成：采样 HDR 纹理 + Bloom 纹理并做 tone mapping。
    // g_Texture 和 g_BloomTexture 使用 DYNAMIC 以便每帧可以更换（Resize 后需要更新）
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "g_BloomTexture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "BloomCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW  = TEXTURE_ADDRESS_CLAMP;

    // 让 HLSL 侧的 g_Texture_sampler / g_BloomTexture_sampler 使用同一个不可变采样器。
    // 注意：D3D12 下需要使用采样器的完整名称（带 _sampler 后缀）
    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, "g_Texture_sampler", sampDesc},
        {SHADER_TYPE_PIXEL, "g_BloomTexture_sampler", sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    fullscreenQuadPSO_.Release();
    fullscreenQuadSRB_.Release();
    device_->CreateGraphicsPipelineState(psoCI, &fullscreenQuadPSO_);
    if (fullscreenQuadPSO_ == nullptr) {
        return false;
    }

    // 创建 Bloom 常量缓冲
    if (bloomConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Bloom Constants";
        cbDesc.Size           = 16; // float4: bloomStrength + padding
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        device_->CreateBuffer(cbDesc, nullptr, &bloomConstants_);
        if (bloomConstants_ == nullptr) {
            return false;
        }
    }

    // 绑定 BloomCB（STATIC 变量）
    if (auto* var = fullscreenQuadPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BloomCB"); var != nullptr) {
        var->Set(bloomConstants_);
    }

    fullscreenQuadPSO_->CreateShaderResourceBinding(&fullscreenQuadSRB_, true);
    return fullscreenQuadSRB_ != nullptr;
}

bool DiligentBackend::CreateStarfieldBuffers(uint32_t starCount) {
    if (device_ == nullptr) {
        return false;
    }
    if (starCount == 0) {
        return false;
    }

    std::mt19937                          gen{1337u};
    std::uniform_real_distribution<float> rnd01(0.0f, 1.0f);

    const uint32_t kPalette[] = {0xE3DAC5u, 0xC9A070u, 0xE3DAC5u, 0xB08D55u};

    std::vector<StarInstance> stars;
    stars.resize(starCount);
    for (uint32_t i = 0; i < starCount; ++i) {
        // 复刻 OpenGL 旧实现：球壳分布
        const float r  = 400.0f + rnd01(gen) * 3000.0f;
        const float th = rnd01(gen) * 6.28318530718f;
        const float ph = std::acos(2.0f * rnd01(gen) - 1.0f);

        StarInstance v{};
        v.Pos[0] = r * std::sin(ph) * std::cos(th);
        v.Pos[1] = r * std::cos(ph);
        v.Pos[2] = r * std::sin(ph) * std::sin(th);

        HexToRGB(kPalette[i % 4], v.Color);

        v.Size = 1.0f + rnd01(gen) * 3.0f;
        v.Seed = rnd01(gen);

        stars[i] = v;
    }

    // Vertex buffer
    {
        BufferDesc vbDesc{};
        vbDesc.Name      = "Starfield VB";
        vbDesc.Usage     = USAGE_IMMUTABLE;
        vbDesc.BindFlags = BIND_VERTEX_BUFFER;
        vbDesc.Size      = static_cast<Uint32>(sizeof(StarInstance) * stars.size());

        BufferData vbData{};
        vbData.pData    = stars.data();
        vbData.DataSize = vbDesc.Size;

        starVB_.Release();
        device_->CreateBuffer(vbDesc, &vbData, &starVB_);
        if (starVB_ == nullptr) {
            return false;
        }
    }

    // Constant buffer (dynamic)
    {
        if (starConstants_ == nullptr) {
            BufferDesc cbDesc{};
            cbDesc.Name           = "Starfield Constants";
            cbDesc.Size           = (sizeof(StarConstants) + 255) & ~255;
            cbDesc.Usage          = USAGE_DYNAMIC;
            cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
            cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

            device_->CreateBuffer(cbDesc, nullptr, &starConstants_);
            if (starConstants_ == nullptr) {
                return false;
            }
        }
    }

    starCount_ = starCount;
    return true;
}

bool DiligentBackend::CreateStarfieldPSO() {
    if (device_ == nullptr || swapChain_ == nullptr || starConstants_ == nullptr) {
        return false;
    }

    const auto sources = GetStarShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "Starfield VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language);
    const auto ps =
        CreateShaderFromSource(device_, "Starfield PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "Starfield PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    const auto& scDesc                      = swapChain_->GetDesc();
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    (void)scDesc;
    psoCI.GraphicsPipeline.RTVFormats[0]     = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat         = TEX_FORMAT_UNKNOWN;
    psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    auto& blendRT          = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blendRT.BlendEnable    = True;
    blendRT.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
    blendRT.DestBlend      = BLEND_FACTOR_ONE;
    blendRT.BlendOp        = BLEND_OPERATION_ADD;
    blendRT.SrcBlendAlpha  = BLEND_FACTOR_ONE;
    blendRT.DestBlendAlpha = BLEND_FACTOR_ONE;
    blendRT.BlendOpAlpha   = BLEND_OPERATION_ADD;

    const LayoutElement layoutElems[] = {
        LayoutElement{0, 0, 3, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{1, 0, 3, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{2, 0, 1, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{3, 0, 1, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
    };
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;
    psoCI.GraphicsPipeline.InputLayout.NumElements    = _countof(layoutElems);

    // 常量缓冲设为静态变量：每帧只更新 buffer 内容。
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "StarConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "StarConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    starPSO_.Release();
    device_->CreateGraphicsPipelineState(psoCI, &starPSO_);
    if (starPSO_ == nullptr) {
        return false;
    }

    if (auto* varVS = starPSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "StarConstants"); varVS != nullptr) {
        varVS->Set(starConstants_);
    } else {
        return false;
    }
    if (auto* varPS = starPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "StarConstants"); varPS != nullptr) {
        varPS->Set(starConstants_);
    } else {
        return false;
    }

    starSRB_.Release();
    starPSO_->CreateShaderResourceBinding(&starSRB_, true);
    return starSRB_ != nullptr;
}

bool DiligentBackend::CreateParticleBuffers(uint32_t maxParticles) {
    if (device_ == nullptr || immediateContext_ == nullptr) {
        SetLastError(L"CreateParticleBuffers: device/context 为空。");
        return false;
    }
    if (maxParticles == 0) {
        SetLastError(L"CreateParticleBuffers: maxParticles=0。");
        return false;
    }

    // OpenGL 版在 ComputeInitSaturn 里用 time(0) 作为随机种子（uSeed）。
    // Diligent 版这里也对齐：避免环过于“统计学完美对称”，导致即使在公转也很难被肉眼感知。
    const uint32_t seed         = static_cast<uint32_t>(std::time(nullptr));
    const auto     cpuParticles = InitSaturnParticlesCPU(maxParticles, seed);
    if (cpuParticles.empty()) {
        SetLastError(L"CreateParticleBuffers: CPU 初始化粒子数组为空。");
        return false;
    }

    const Uint64 bufferSize = static_cast<Uint64>(sizeof(SaturnParticle)) * static_cast<Uint64>(cpuParticles.size());

    for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
        BufferDesc bufDesc{};
        bufDesc.Name              = "Saturn Particles";
        bufDesc.Size              = bufferSize;
        bufDesc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        bufDesc.Usage             = USAGE_DEFAULT;
        bufDesc.Mode              = BUFFER_MODE_STRUCTURED;
        bufDesc.ElementByteStride = sizeof(SaturnParticle);

        BufferData initData{};
        initData.pData    = cpuParticles.data();
        initData.DataSize = bufferSize;

        particleUAVs_[i].Release();
        particleSRVs_[i].Release();
        particleBuffers_[i].Release();

        device_->CreateBuffer(bufDesc, &initData, &particleBuffers_[i]);
        if (particleBuffers_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Saturn Particles) 失败（可能显存不足）。");
            return false;
        }

        particleSRVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE);
        particleUAVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
        if (particleSRVs_[i] == nullptr || particleUAVs_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffers: 获取粒子 SRV/UAV 失败。");
            return false;
        }
    }

    // 常量缓冲（每帧更新）
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Constants";
        cbDesc.Size           = (sizeof(StarConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleConstants_);
        if (particleConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Particle Constants) 失败。");
            return false;
        }
    }

    // Compute 常量缓冲（每帧更新）
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Compute Constants";
        cbDesc.Size           = (sizeof(ParticleComputeConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleComputeConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleComputeConstants_);
        if (particleComputeConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Particle Compute Constants) 失败。");
            return false;
        }
    }

    // Indirect draw args（复刻 OpenGL 的 glDrawArraysIndirect）：4x uint32
    // { NumVertices, NumInstances, StartVertexLocation, FirstInstanceLocation }
    {
        uint32_t args[4] = {6u, maxParticles, 0u, 0u};

        BufferDesc bufDesc{};
        bufDesc.Name      = "Particle Indirect Draw Args";
        bufDesc.Size      = sizeof(args);
        bufDesc.BindFlags = BIND_INDIRECT_DRAW_ARGS;
        // 注意：Indirect args 必须是 GPU 可读的缓冲；不要用可映射的动态缓冲（部分后端会直接创建失败）。
        // 后续如果要做动态 LOD，需要更新 args 时，用 IDeviceContext::UpdateBuffer() 写入即可。
        bufDesc.Usage          = USAGE_DEFAULT;
        bufDesc.CPUAccessFlags = CPU_ACCESS_NONE;

        BufferData init{};
        init.pData    = args;
        init.DataSize = sizeof(args);

        particleIndirectArgs_.Release();
        device_->CreateBuffer(bufDesc, &init, &particleIndirectArgs_);
        if (particleIndirectArgs_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Indirect Draw Args) 失败。");
            return false;
        }

        // D3D12: 显式将间接参数缓冲区转换到正确的初始状态
        StateTransitionDesc barrier{};
        barrier.pResource      = particleIndirectArgs_;
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
        barrier.NewState       = RESOURCE_STATE_INDIRECT_ARGUMENT;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    particleCount_ = maxParticles;
    // 使用正确的三缓冲初始化：确保 render、read、write 指向三个不同的缓冲区
    // 这样可以避免任何读写冲突
    // - buffer[0]：初始用于 read（计算输入）
    // - buffer[1]：初始用于 write（计算输出）
    // - buffer[2]：初始用于 render（渲染）
    particleRenderIdx_ = 2;
    particleReadIdx_   = 0;
    particleWriteIdx_  = 1;
    return true;
}

bool DiligentBackend::CreateParticlePSO() {
    if (device_ == nullptr || swapChain_ == nullptr || particleConstants_ == nullptr) {
        return false;
    }

    const auto sources = GetSaturnParticleShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "SaturnParticle VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language);
    const auto ps =
        CreateShaderFromSource(device_, "SaturnParticle PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnParticle PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    const auto& scDesc                      = swapChain_->GetDesc();
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    (void)scDesc;
    psoCI.GraphicsPipeline.RTVFormats[0]                = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat                    = TEX_FORMAT_UNKNOWN;
    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    auto& blendRT          = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blendRT.BlendEnable    = True;
    blendRT.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
    blendRT.DestBlend      = BLEND_FACTOR_ONE;
    blendRT.BlendOp        = BLEND_OPERATION_ADD;
    blendRT.SrcBlendAlpha  = BLEND_FACTOR_ONE;
    blendRT.DestBlendAlpha = BLEND_FACTOR_ONE;
    blendRT.BlendOpAlpha   = BLEND_OPERATION_ADD;

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        // g_Particles 需要每帧更换（三缓冲轮转），必须使用 DYNAMIC 类型
        {SHADER_TYPE_VERTEX, "g_Particles", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    particlePSO_.Release();
    device_->CreateGraphicsPipelineState(psoCI, &particlePSO_);
    if (particlePSO_ == nullptr) {
        return false;
    }

    if (auto* varVS = particlePSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "ParticleConstants");
        varVS != nullptr) {
        varVS->Set(particleConstants_);
    } else {
        return false;
    }
    if (auto* varPS = particlePSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "ParticleConstants"); varPS != nullptr) {
        varPS->Set(particleConstants_);
    } else {
        return false;
    }

    // g_Particles 是 DYNAMIC 变量，需要在每帧渲染前通过 SRB 设置，此处不绑定

    particleSRB_.Release();
    particlePSO_->CreateShaderResourceBinding(&particleSRB_, true);
    return particleSRB_ != nullptr;
}

bool DiligentBackend::CreateParticleComputePSO() {
    if (device_ == nullptr || immediateContext_ == nullptr || particleComputeConstants_ == nullptr) {
        return false;
    }

    const auto csSrc = GetSaturnComputeShaderSource(backend_);
    if (csSrc.Source == nullptr) {
        OutputDebugStringA("[CreateParticleComputePSO] GetSaturnComputeShaderSource returned nullptr\n");
        return false;
    }

    const auto cs =
        CreateShaderFromSource(device_, "SaturnCompute CS", SHADER_TYPE_COMPUTE, csSrc.Source, csSrc.Language);
    if (cs == nullptr) {
        OutputDebugStringA("[CreateParticleComputePSO] Compute shader compilation failed\n");
        return false;
    }

    ComputePipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnCompute PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;

    // 变量声明顺序必须与 Vulkan GLSL 着色器中的 binding 索引一致：
    // - g_ParticlesIn: binding=0 (DYNAMIC - 需要每帧更换缓冲区)
    // - g_ParticlesOut: binding=1 (DYNAMIC - 需要每帧更换缓冲区)
    // - ComputeConstants: binding=2 (STATIC)
    // 注意：MUTABLE 只能在 SRB 创建后设置一次，DYNAMIC 才能每帧更新！
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_COMPUTE, "g_ParticlesIn", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "g_ParticlesOut", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "ComputeConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pCS = cs;

    particleComputePSO_.Release();
    particleComputeSRB_.Release();
    device_->CreateComputePipelineState(psoCI, &particleComputePSO_);
    if (particleComputePSO_ == nullptr) {
        OutputDebugStringA("[CreateParticleComputePSO] CreateComputePipelineState failed\n");
        return false;
    }

    if (auto* var = particleComputePSO_->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "ComputeConstants");
        var != nullptr) {
        var->Set(particleComputeConstants_);
    } else {
        OutputDebugStringA("[CreateParticleComputePSO] GetStaticVariableByName(ComputeConstants) returned nullptr\n");
        return false;
    }

    particleComputePSO_->CreateShaderResourceBinding(&particleComputeSRB_, true);
    if (particleComputeSRB_ == nullptr) {
        OutputDebugStringA("[CreateParticleComputePSO] CreateShaderResourceBinding failed\n");
    }
    return particleComputeSRB_ != nullptr;
}

bool DiligentBackend::CreateOffscreenRenderTarget(SurfaceSize size) {
    if (device_ == nullptr || swapChain_ == nullptr) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return false;
    }

    const auto& scDesc = swapChain_->GetDesc();

    TextureDesc texDesc{};
    texDesc.Name      = "Offscreen Color";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = size.Width;
    texDesc.Height    = size.Height;
    texDesc.MipLevels = 1;
    (void)scDesc;
    // 与 OpenGL 旧版 FBO 对齐：R11G11B10F HDR（便于加法混合后在最终合成阶段做 tone mapping，避免过曝）。
    texDesc.Format    = kOffscreenColorFormat;
    texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_DEFAULT;

    offscreenRTV_.Release();
    offscreenSRV_.Release();
    offscreenColor_.Release();

    device_->CreateTexture(texDesc, nullptr, &offscreenColor_);
    if (offscreenColor_ == nullptr) {
        return false;
    }

    offscreenRTV_ = offscreenColor_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    offscreenSRV_ = offscreenColor_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (offscreenSRV_ == nullptr) {
        return false;
    }
    return offscreenRTV_ != nullptr;
}

bool DiligentBackend::CreateSevenSegmentPSO() {
    if (device_ == nullptr) {
        return false;
    }

    const bool  isVulkan = (backend_ == Backend::Vulkan);
    const char* vsSource = isVulkan ? kSevenSegGlslVS : kSevenSegHlslVS;
    const char* psSource = isVulkan ? kSevenSegGlslPS : kSevenSegHlslPS;
    const auto  lang     = isVulkan ? SHADER_SOURCE_LANGUAGE_GLSL : SHADER_SOURCE_LANGUAGE_HLSL;

    RefCntAutoPtr<IShader> vs, ps;
    {
        ShaderCreateInfo sci{};
        sci.SourceLanguage  = lang;
        sci.Desc.ShaderType = SHADER_TYPE_VERTEX;
        sci.Desc.Name       = "SevenSegment VS";
        sci.EntryPoint      = "main";
        sci.Source          = vsSource;
        device_->CreateShader(sci, &vs);
        if (vs == nullptr) {
            return false;
        }
    }
    {
        ShaderCreateInfo sci{};
        sci.SourceLanguage  = lang;
        sci.Desc.ShaderType = SHADER_TYPE_PIXEL;
        sci.Desc.Name       = "SevenSegment PS";
        sci.EntryPoint      = "main";
        sci.Source          = psSource;
        device_->CreateShader(sci, &ps);
        if (ps == nullptr) {
            return false;
        }
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SevenSegment PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    // 顶点输入：float2 位置
    LayoutElement layoutElems[] = {
        {0, 0, 2, VT_FLOAT32, False},
    };
    psoCI.GraphicsPipeline.InputLayout.NumElements    = 1;
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;
    psoCI.GraphicsPipeline.PrimitiveTopology          = PRIMITIVE_TOPOLOGY_LINE_LIST;

    // 渲染目标格式
    const auto& scDesc                      = swapChain_->GetDesc();
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = scDesc.ColorBufferFormat;
    psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN; // 不使用深度

    // Alpha 混合（线条不透明，但保持一致性）
    auto& rt0                 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt0.BlendEnable           = True;
    rt0.SrcBlend              = BLEND_FACTOR_SRC_ALPHA;
    rt0.DestBlend             = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOp               = BLEND_OPERATION_ADD;
    rt0.SrcBlendAlpha         = BLEND_FACTOR_ONE;
    rt0.DestBlendAlpha        = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOpAlpha          = BLEND_OPERATION_ADD;
    rt0.RenderTargetWriteMask = COLOR_MASK_ALL;

    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // 资源布局
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "SevenSegCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.Variables    = vars;
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    device_->CreateGraphicsPipelineState(psoCI, &sevenSegPSO_);
    if (sevenSegPSO_ == nullptr) {
        return false;
    }

    // 创建常量缓冲
    BufferDesc cbDesc{};
    cbDesc.Name           = "SevenSegment Constants";
    cbDesc.Size           = 64 + 16 + 16; // mat4 + vec4 + vec4 (padding to 16-byte alignment)
    cbDesc.Usage          = USAGE_DYNAMIC;
    cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(cbDesc, nullptr, &sevenSegConstants_);
    if (sevenSegConstants_ == nullptr) {
        return false;
    }

    // 绑定常量缓冲
    if (auto* var = sevenSegPSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "SevenSegCB"); var != nullptr) {
        var->Set(sevenSegConstants_);
    }

    sevenSegPSO_->CreateShaderResourceBinding(&sevenSegSRB_, true);
    return sevenSegSRB_ != nullptr;
}

bool DiligentBackend::CreateSevenSegmentBuffers() {
    if (device_ == nullptr) {
        return false;
    }

    // 标准化坐标 (0,0) 到 (1,1.8)，与 OpenGL 版一致
    const float w = 1.0f, h = 1.8f;
    const float p[6][2] = {{0, h}, {w, h}, {w, h / 2}, {w, 0}, {0, 0}, {0, h / 2}};

    for (int num = 0; num < 10; ++num) {
        std::vector<float> verts;
        auto               addLine = [&](int i1, int i2) {
            verts.push_back(p[i1][0]);
            verts.push_back(p[i1][1]);
            verts.push_back(p[i2][0]);
            verts.push_back(p[i2][1]);
        };

        if (kDigits[num][0]) {
            addLine(0, 1); // top
        }
        if (kDigits[num][1]) {
            addLine(1, 2); // top-right
        }
        if (kDigits[num][2]) {
            addLine(2, 3); // bottom-right
        }
        if (kDigits[num][3]) {
            addLine(3, 4); // bottom
        }
        if (kDigits[num][4]) {
            addLine(4, 5); // bottom-left
        }
        if (kDigits[num][5]) {
            addLine(5, 0); // top-left
        }
        if (kDigits[num][6]) {
            addLine(5, 2); // middle
        }

        sevenSegVertexCount_[num] = static_cast<uint32_t>(verts.size() / 2);

        BufferDesc vbDesc{};
        vbDesc.Name      = "SevenSegment VB";
        vbDesc.Size      = verts.size() * sizeof(float);
        vbDesc.Usage     = USAGE_IMMUTABLE;
        vbDesc.BindFlags = BIND_VERTEX_BUFFER;

        BufferData initData{};
        initData.pData    = verts.data();
        initData.DataSize = vbDesc.Size;

        sevenSegVB_[num].Release();
        device_->CreateBuffer(vbDesc, &initData, &sevenSegVB_[num]);
        if (sevenSegVB_[num] == nullptr) {
            return false;
        }
    }

    return true;
}

void DiligentBackend::UpdateFullscreenQuadBindings() {
    // g_Texture 和 g_BloomTexture 现在是 DYNAMIC 变量，不需要重新创建 SRB
    // 只需确保在 BlitOffscreenToBackBuffer 中每帧绑定正确的纹理即可
    // 此函数保留用于 PSO 创建后的初始化工作（如果需要）
}

void DiligentBackend::SimulateParticles(float dt, float handScale, float handHas) {
    if (immediateContext_ == nullptr || particleComputePSO_ == nullptr || particleComputeSRB_ == nullptr ||
        particleComputeConstants_ == nullptr) {
        OutputDebugStringA("[SimulateParticles] Early return: nullptr check failed\n");
        return;
    }
    if (particleCount_ == 0) {
        OutputDebugStringA("[SimulateParticles] Early return: particleCount_ == 0\n");
        return;
    }
    // 复刻 OpenGL 的三缓冲索引用法（见 OpenGL: DoubleBufferSSBO::Swap）：
    // - readIdx：本帧计算着色器的输入
    // - writeIdx：本帧计算着色器的输出
    // - renderIdx：本帧渲染使用的数据（Swap 后等于上一帧的 readIdx）
    if (particleSRVs_[particleReadIdx_] == nullptr || particleUAVs_[particleWriteIdx_] == nullptr) {
        OutputDebugStringA("[SimulateParticles] Early return: SRV/UAV nullptr\n");
        return;
    }

    // 防止 dt 为 0 导致粒子静止（尤其是第一帧）
    if (dt < 0.001f) {
        dt = 0.016f;
    }

    // 更新 Compute 常量（1:1 对齐 OpenGL uniform：uDt/uHandScale/uHandHas/uParticleCount）
    {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(particleComputeConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            auto* cb          = static_cast<ParticleComputeConstants*>(mapped);
            cb->Dt            = dt;
            cb->HandScale     = handScale;
            cb->HandHas       = handHas;
            cb->ParticleCount = particleCount_;
            immediateContext_->UnmapBuffer(particleComputeConstants_, MAP_WRITE);
        }
    }

    // 计算着色器输入使用 readIdx，与 OpenGL 版本一致（读 readIdx，写 writeIdx）。
    if (auto* varIn = particleComputeSRB_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_ParticlesIn"); varIn != nullptr) {
        varIn->Set(particleSRVs_[particleReadIdx_]);
    } else {
        return;
    }

    if (auto* varOut = particleComputeSRB_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_ParticlesOut");
        varOut != nullptr) {
        varOut->Set(particleUAVs_[particleWriteIdx_]);
    } else {
        return;
    }

    immediateContext_->SetPipelineState(particleComputePSO_);
    immediateContext_->CommitShaderResources(particleComputeSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs dispatch{};
    dispatch.ThreadGroupCountX = (particleCount_ + 255u) / 256u;
    dispatch.ThreadGroupCountY = 1;
    dispatch.ThreadGroupCountZ = 1;
    immediateContext_->DispatchCompute(dispatch);

    // 显式的资源状态转换：确保 UAV 写入对后续的 SRV 读取可见
    // 这是等价于 OpenGL 的 glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT)
    {
        StateTransitionDesc barrier{};
        barrier.pResource      = particleBuffers_[particleWriteIdx_];
        barrier.OldState       = RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.NewState       = RESOURCE_STATE_SHADER_RESOURCE;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    // 三缓冲轮转（完全复刻 OpenGL 版本 DoubleBufferSSBO::Swap）：
    // - renderIdx <- readIdx：上一帧计算完成的数据变为渲染数据
    // - readIdx <- writeIdx：本帧写入的变为下一帧读取（计算输入）
    // - writeIdx <- oldRender：渲染完的缓冲变为下一帧写入目标
    //
    // 这意味着渲染总是落后计算一帧，这是正常的三缓冲流水线行为。
    // 从第二帧开始，渲染的数据就是经过计算更新的。
    const uint32_t oldRender = particleRenderIdx_;
    particleRenderIdx_       = particleReadIdx_;
    particleReadIdx_         = particleWriteIdx_;
    particleWriteIdx_        = oldRender;

    // g_Particles 是 DYNAMIC 变量，将在 RenderOffscreen 的 CommitShaderResources 前通过 SRB 设置
}

void DiligentBackend::RenderClear() {
    if (swapChain_ == nullptr || immediateContext_ == nullptr) {
        return;
    }

    ITextureView* pRTV = swapChain_->GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }

    ITextureView* pDSV = swapChain_->GetDepthBufferDSV();

    immediateContext_->SetRenderTargets(1, &pRTV, pDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    const auto& scDesc = swapChain_->GetDesc();
    Viewport    vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(scDesc.Width);
    vp.Height   = static_cast<float>(scDesc.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, scDesc.Width, scDesc.Height);

    // 颜色随时间变化留到后续；此处用固定色验证呈现链路。
    const float clearColor[4] = {0.05f, 0.07f, 0.10f, 1.0f};
    immediateContext_->ClearRenderTarget(pRTV, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (pDSV != nullptr) {
        immediateContext_->ClearDepthStencil(pDSV, CLEAR_DEPTH_FLAG, 1.0f, 0,
                                             RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

void DiligentBackend::RenderOffscreen() {
    if (immediateContext_ == nullptr) {
        return;
    }

    OutputDebugStringA("  RenderOffscreen: Setup RT start\n");

    ITextureView* pRTV = offscreenRTV_;
    immediateContext_->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 以实际离屏 RT 尺寸为准，避免 Resize/DPI 缩放导致的"像素尺寸 -> NDC"换算偏差（会直接影响星空/粒子"密度"观感）。
    uint32_t rtW = surfaceSize_.Width;
    uint32_t rtH = surfaceSize_.Height;
    if (offscreenColor_ != nullptr) {
        const auto& rtDesc = offscreenColor_->GetDesc();
        rtW                = rtDesc.Width;
        rtH                = rtDesc.Height;
    }

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(rtW);
    vp.Height   = static_cast<float>(rtH);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, rtW, rtH);

    // 星空背景：先清为黑色，再加法混合叠加星点（更接近 OpenGL 旧实现观感）。
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    immediateContext_->ClearRenderTarget(pRTV, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 星空点精灵（加法混合）
    if (starPSO_ != nullptr && starVB_ != nullptr && starSRB_ != nullptr && starCount_ > 0) {
        // 更新常量（view/proj/model + 视口 + 时间）
        const auto now   = std::chrono::steady_clock::now();
        const auto secsF = std::chrono::duration<float>(now - startTime_).count();

        {
            PVoid mapped = nullptr;
            immediateContext_->MapBuffer(starConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
            if (mapped != nullptr) {
                auto* cb = static_cast<StarConstants*>(mapped);

                const float aspect = rtH > 0 ? (static_cast<float>(rtW) / static_cast<float>(rtH)) : 1.0f;

                const Mat4Rows view  = LookAtRH({0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
                const Mat4Rows proj  = PerspectiveRH_OpenGL(1.047f, aspect, 1.0f, 10000.0f);
                const Mat4Rows model = RotationY(secsF * 0.005f);

                for (int c = 0; c < 4; ++c) {
                    cb->ViewRow0[c] = view.Row[0][c];
                    cb->ViewRow1[c] = view.Row[1][c];
                    cb->ViewRow2[c] = view.Row[2][c];
                    cb->ViewRow3[c] = view.Row[3][c];

                    cb->ProjRow0[c] = proj.Row[0][c];
                    cb->ProjRow1[c] = proj.Row[1][c];
                    cb->ProjRow2[c] = proj.Row[2][c];
                    cb->ProjRow3[c] = proj.Row[3][c];

                    cb->ModelRow0[c] = model.Row[0][c];
                    cb->ModelRow1[c] = model.Row[1][c];
                    cb->ModelRow2[c] = model.Row[2][c];
                    cb->ModelRow3[c] = model.Row[3][c];
                }

                cb->ViewportParams[0] = rtW > 0 ? (2.0f / static_cast<float>(rtW)) : 0.0f;
                cb->ViewportParams[1] = rtH > 0 ? (2.0f / static_cast<float>(rtH)) : 0.0f;
                cb->ViewportParams[2] = static_cast<float>(rtW);
                cb->ViewportParams[3] = static_cast<float>(rtH);

                cb->TimeParams[0] = secsF;
                immediateContext_->UnmapBuffer(starConstants_, MAP_WRITE);
            }
        }

        immediateContext_->SetPipelineState(starPSO_);

        IBuffer* pVBs[]    = {starVB_};
        Uint64   offsets[] = {0};
        immediateContext_->SetVertexBuffers(0, 1, pVBs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        immediateContext_->CommitShaderResources(starSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DrawAttribs starsDraw{};
        starsDraw.NumVertices  = 6;
        starsDraw.NumInstances = starCount_;
        starsDraw.Flags        = DRAW_FLAG_VERIFY_ALL;
        immediateContext_->Draw(starsDraw);
    }

    // 土星粒子（阶段 3：先 CPU 初始化，渲染验证）
    if (particlePSO_ != nullptr && particleSRB_ != nullptr && particleConstants_ != nullptr && particleCount_ > 0) {
        const auto now   = std::chrono::steady_clock::now();
        const auto secsF = std::chrono::duration<float>(now - startTime_).count();

        // 改为与帧率无关：保持“当前 180fps 下”的播放速度 + 平滑强度。
        // - 旧版：autoTime 每帧 +0.005 -> 180fps 等价每秒 +0.9
        // - 旧版：每帧 lerpFactor=0.08 -> dt 下等效 alpha = 1 - (1-0.08)^(dt*180)
        float dt = 0.0f;
        if (lastAnimTime_ != std::chrono::steady_clock::time_point{}) {
            dt = std::chrono::duration<float>(now - lastAnimTime_).count();
        }
        lastAnimTime_ = now;
        if (dt < 0.0f) {
            dt = 0.0f;
        }
        if (dt > 0.2f) {
            dt = 0.2f;
        }

        animAutoTime_ += dt * (0.005f * 180.0f);

        const float targetScale = 1.0f + std::sin(animAutoTime_) * 0.2f;
        const float targetRotX  = 0.4f + std::sin(animAutoTime_ * 0.3f) * 0.15f;
        const float targetRotY  = 0.0f;

        const float alpha = 1.0f - std::pow(1.0f - 0.08f, dt * 180.0f);
        animScale_        = animScale_ + (targetScale - animScale_) * alpha;
        animRotX_         = animRotX_ + (targetRotX - animRotX_) * alpha;
        animRotY_         = animRotY_ + (targetRotY - animRotY_) * alpha;

        // 阶段 3（第 2 步）：接入 GPU ComputeSaturn（物理模拟）并用三缓冲轮转避免读写冲突。
        // 当前没有手势追踪，uHandHas=0。
        SimulateParticles(dt, animScale_, 0.0f);

        {
            PVoid mapped = nullptr;
            immediateContext_->MapBuffer(particleConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
            if (mapped != nullptr) {
                auto* cb = static_cast<StarConstants*>(mapped);

                const float aspect = rtH > 0 ? (static_cast<float>(rtW) / static_cast<float>(rtH)) : 1.0f;

                const Mat4Rows view = LookAtRH({0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
                const Mat4Rows proj = PerspectiveRH_OpenGL(1.047f, aspect, 1.0f, 10000.0f);

                // 复刻 OpenGL 的角度逻辑：mSat = Rx(rotX) * Ry(rotY) * Rz(0.466)
                const float uScale = animScale_;
                const float rotX   = animRotX_;
                const float rotY   = animRotY_;
                const float rotZ   = 0.466f;

                const Mat4Rows model = Mul(Mul(RotationX(rotX), RotationY(rotY)), RotationZ(rotZ));

                for (int c = 0; c < 4; ++c) {
                    cb->ViewRow0[c] = view.Row[0][c];
                    cb->ViewRow1[c] = view.Row[1][c];
                    cb->ViewRow2[c] = view.Row[2][c];
                    cb->ViewRow3[c] = view.Row[3][c];

                    cb->ProjRow0[c] = proj.Row[0][c];
                    cb->ProjRow1[c] = proj.Row[1][c];
                    cb->ProjRow2[c] = proj.Row[2][c];
                    cb->ProjRow3[c] = proj.Row[3][c];

                    cb->ModelRow0[c] = model.Row[0][c];
                    cb->ModelRow1[c] = model.Row[1][c];
                    cb->ModelRow2[c] = model.Row[2][c];
                    cb->ModelRow3[c] = model.Row[3][c];
                }

                cb->ViewportParams[0] = rtW > 0 ? (2.0f / static_cast<float>(rtW)) : 0.0f;
                cb->ViewportParams[1] = rtH > 0 ? (2.0f / static_cast<float>(rtH)) : 0.0f;
                cb->ViewportParams[2] = static_cast<float>(rtW);
                cb->ViewportParams[3] = static_cast<float>(rtH);

                cb->TimeParams[0] = secsF;

                // 暂时没有动态 LOD 系统，先用 pixelRatio=1，densityComp 按旧公式从粒子数推导（更接近原版观感）。
                const float pixelRatio  = 1.0f;
                const float ratio       = static_cast<float>(particleCount_) / 1200000.0f;
                const float densityComp = 0.6f / std::pow(std::max(ratio, 0.0001f), 0.7f) / std::pow(pixelRatio, 0.5f);

                cb->RenderParams[0] = uScale;
                cb->RenderParams[1] = pixelRatio;
                cb->RenderParams[2] = static_cast<float>(rtH);
                cb->RenderParams[3] = densityComp;

                immediateContext_->UnmapBuffer(particleConstants_, MAP_WRITE);
            }
        }

        immediateContext_->SetPipelineState(particlePSO_);
        // 不使用顶点缓冲（SV_VertexID/InstanceID 生成），但需要清掉之前的绑定状态。
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        // 每帧更新 DYNAMIC 变量 g_Particles（三缓冲轮转后指向新的渲染缓冲区）
        if (particleSRVs_[particleRenderIdx_] != nullptr) {
            if (auto* var = particleSRB_->GetVariableByName(SHADER_TYPE_VERTEX, "g_Particles"); var != nullptr) {
                var->Set(particleSRVs_[particleRenderIdx_]);
            }
        }

        immediateContext_->CommitShaderResources(particleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // 复刻 OpenGL：glDrawArraysIndirect(GL_POINTS, nullptr)
        if (particleIndirectArgs_ != nullptr) {
            DrawIndirectAttribs ia{};
            ia.pAttribsBuffer                   = particleIndirectArgs_;
            ia.Flags                            = DRAW_FLAG_VERIFY_ALL;
            ia.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            immediateContext_->DrawIndirect(ia);
        }
    }
}

void DiligentBackend::BlitOffscreenToBackBuffer() {
    if (swapChain_ == nullptr || immediateContext_ == nullptr || fullscreenQuadPSO_ == nullptr ||
        fullscreenQuadSRB_ == nullptr || offscreenSRV_ == nullptr) {
        return;
    }

    ITextureView* pBackBufferRTV = swapChain_->GetCurrentBackBufferRTV();
    if (pBackBufferRTV == nullptr) {
        return;
    }

    // 全屏合成（只做 tone mapping，不混合 bloom）
    immediateContext_->SetRenderTargets(1, &pBackBufferRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    const auto& scDesc = swapChain_->GetDesc();
    Viewport    vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(scDesc.Width);
    vp.Height   = static_cast<float>(scDesc.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, scDesc.Width, scDesc.Height);

    // 设置 Bloom 强度为 0（不使用全局 bloom）
    if (bloomConstants_ != nullptr) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BloomCB {
                float strength;
                float pad[3];
            };

            auto* cb     = static_cast<BloomCB*>(mapped);
            cb->strength = 0.0f; // 不使用全局 bloom
            cb->pad[0] = cb->pad[1] = cb->pad[2] = 0.0f;
            immediateContext_->UnmapBuffer(bloomConstants_, MAP_WRITE);
        }
    }

    // 只绑定原始 HDR 纹理（不混合 bloom）
    if (auto* var = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture"); var != nullptr) {
        var->Set(offscreenSRV_);
    }
    // Bloom 纹理也绑定原图（因为 strength=0，不会有影响）
    if (auto* var = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_BloomTexture"); var != nullptr) {
        var->Set(offscreenSRV_);
    }

    immediateContext_->SetPipelineState(fullscreenQuadPSO_);
    immediateContext_->CommitShaderResources(fullscreenQuadSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = DRAW_FLAG_VERIFY_ALL;
    immediateContext_->Draw(draw);
}

void DiligentBackend::RenderFrame() {
    if (swapChain_ == nullptr) {
        return;
    }

    // 计算帧时间和 FPS（移动平均）
    const auto now     = std::chrono::steady_clock::now();
    float      frameDt = 0.0f;
    if (lastFrameTime_ != std::chrono::steady_clock::time_point{}) {
        frameDt = std::chrono::duration<float>(now - lastFrameTime_).count();
        if (frameDt > 0.0f && frameDt < 1.0f) {
            fpsSamples_[fpsSampleIndex_] = 1.0f / frameDt;
            fpsSampleIndex_              = (fpsSampleIndex_ + 1) % kFpsSampleCount;

            // 计算平均 FPS
            float sum = 0.0f;
            for (int i = 0; i < kFpsSampleCount; ++i) {
                sum += fpsSamples_[i];
            }
            currentFps_ = sum / static_cast<float>(kFpsSampleCount);

            // FPS 历史曲线采样（低频）
            fpsHistorySampleTimer_ += frameDt;
            if (fpsHistorySampleTimer_ >= kFpsHistorySampleInterval) {
                fpsHistorySampleTimer_        = 0.0f;
                fpsHistory_[fpsHistoryIndex_] = currentFps_;
                fpsHistoryIndex_              = (fpsHistoryIndex_ + 1) % kFpsHistorySize;
                // 触发滚动动画
                fpsGraphScrollOffset_ = 1.0f;
            }

            // 滚动动画衰减
            if (fpsGraphScrollOffset_ > 0.001f) {
                const float kScrollAnimSpeed = 12.0f;
                fpsGraphScrollOffset_ *= expf(-kScrollAnimSpeed * frameDt);
            } else {
                fpsGraphScrollOffset_ = 0.0f;
            }
        }
    }
    lastFrameTime_ = now;

    // ImGui 新帧
    if (imgui_) {
        imgui_->NewFrame();

        // MD3 新帧
        MD3::BeginFrame(frameDt > 0.0f ? frameDt : (1.0f / 60.0f));
        MD3::SetDarkMode(appState_->ui.isDarkMode);

        // Debug 窗口 - 使用 MD3 无标题栏样式
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(1200, 1200));
        constexpr ImGuiWindowFlags kDebugWindowFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
        ImGui::Begin("Debug", nullptr, kDebugWindowFlags);

        // 自定义标题栏
        constexpr float kTitleBarHeight = 40.0f;
        MD3::WindowTitleBar("Debug", nullptr);

        // 绘制窗口背景
        {
            ImVec2      pos   = ImGui::GetWindowPos();
            ImVec2      size  = ImGui::GetWindowSize();
            ImDrawList* dl    = ImGui::GetWindowDrawList();
            ImGuiStyle& style = ImGui::GetStyle();

            auto&  colors       = MD3::GetContext().colors;
            float  cornerRadius = style.WindowRounding;
            ImVec2 endPos       = ImVec2(pos.x + size.x, pos.y + size.y);

            // 之前在这里有模糊背景逻辑，已被移除
            ImVec4 bgCol = colors.surfaceContainerLow;
            bgCol.w      = 0.95f;
            dl->AddRectFilled(pos, endPos, ImGui::GetColorU32(bgCol), cornerRadius);
        }

        // ========== 性能区域 ==========
        if (MD3::BeginCollapsingHeader("Performance", true)) {
            // 两列布局的辅助 lambda
            auto TwoColumnText = [](const char* label, const char* fmt, ...) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                va_list args;
                va_start(args, fmt);
                char buf[128];
                vsnprintf(buf, sizeof(buf), fmt, args);
                va_end(args);
                ImGui::Text("%s", buf);
            };

            if (ImGui::BeginTable("PerfTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // FPS（带颜色）- 使用 MD3 色彩方案
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("FPS");
                ImGui::TableNextColumn();
                auto&  fpsColors = MD3::GetContext().colors;
                ImVec4 fpsColor  = (currentFps_ >= 50.0f) ? fpsColors.primary
                                 : (currentFps_ >= 30.0f) ? fpsColors.tertiary
                                                          : fpsColors.error;
                ImGui::TextColored(fpsColor, "%.1f", currentFps_);

                TwoColumnText("Particles", "%u", particleCount_);
                TwoColumnText("Resolution", "%u x %u", surfaceSize_.Width, surfaceSize_.Height);
                TwoColumnText("Backend", "%s", backend_ == Backend::D3D12 ? "D3D12" : "Vulkan");

                ImGui::EndTable();
            }

            // FPS 历史曲线
            ImGui::Dummy(ImVec2(0, 5));

            // 获取历史数据，从最旧到最新
            auto getValue = [&](int logicalIdx) -> float {
                int actualIdx = (fpsHistoryIndex_ + logicalIdx) % kFpsHistorySize;
                return fpsHistory_[actualIdx];
            };

            // 计算 Y 轴范围
            float dataMin = getValue(0);
            float dataMax = getValue(0);
            for (int i = 1; i < kFpsHistorySize; i++) {
                float v = getValue(i);
                if (v > 0.0f) {
                    if (v < dataMin || dataMin <= 0.0f) {
                        dataMin = v;
                    }
                    if (v > dataMax) {
                        dataMax = v;
                    }
                }
            }

            // 设置最小显示范围
            const float MIN_DISPLAY_RANGE = 30.0f;
            float       dataRange         = dataMax - dataMin;
            if (dataRange < MIN_DISPLAY_RANGE) {
                float center = (dataMax + dataMin) * 0.5f;
                dataMin      = center - MIN_DISPLAY_RANGE * 0.5f;
                dataMax      = center + MIN_DISPLAY_RANGE * 0.5f;
            }

            // 添加边距
            float margin    = (dataMax - dataMin) * 0.1f;
            float targetMin = dataMin - margin;
            float targetMax = dataMax + margin;
            if (targetMin < 0.0f) {
                targetMin = 0.0f;
            }

            // Y 轴范围动画 - 平滑过渡
            const float kAnimSpeed = 8.0f;
            float       animDt     = ImGui::GetIO().DeltaTime;
            if (fpsGraphFirstFrame_) {
                fpsGraphAnimMinVal_ = targetMin;
                fpsGraphAnimMaxVal_ = targetMax;
                fpsGraphFirstFrame_ = false;
            } else {
                float decay         = expf(-kAnimSpeed * animDt);
                fpsGraphAnimMinVal_ = fpsGraphAnimMinVal_ * decay + targetMin * (1.0f - decay);
                fpsGraphAnimMaxVal_ = fpsGraphAnimMaxVal_ * decay + targetMax * (1.0f - decay);
            }

            float minVal   = fpsGraphAnimMinVal_;
            float maxVal   = fpsGraphAnimMaxVal_;
            float valRange = maxVal - minVal;
            if (valRange < 1.0f) {
                valRange = 1.0f;
            }

            // 绘图区域
            ImVec2      plotSize(ImGui::GetContentRegionAvail().x, 50);
            ImVec2      plotPos  = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // 背景
            auto& colors    = MD3::GetContext().colors;
            ImU32 bgColor   = ImGui::GetColorU32(ImGuiCol_FrameBg);
            ImU32 lineColor = ImGui::GetColorU32(colors.primary);
            ImU32 axisColor = ImGui::GetColorU32(colors.onSurfaceVariant);
            drawList->AddRectFilled(plotPos, ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y), bgColor);

            // 裁剪区域
            drawList->PushClipRect(plotPos, ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y), true);

            // 转换坐标（含滚动动画）
            auto toScreen = [&](float logicalX, float val) -> ImVec2 {
                // 应用滚动偏移 - 新数据点从右侧滑入
                float adjustedX  = logicalX - fpsGraphScrollOffset_;
                float x          = plotPos.x + (adjustedX / (float)(kFpsHistorySize - 1)) * plotSize.x;
                float clampedVal = val < minVal ? minVal : (val > maxVal ? maxVal : val);
                float y          = plotPos.y + plotSize.y - ((clampedVal - minVal) / valRange) * plotSize.y;
                return ImVec2(x, y);
            };

            // 绘制曲线（Catmull-Rom 样条插值）
            ImVector<ImVec2> dataPoints;
            dataPoints.reserve(kFpsHistorySize);
            for (int i = 0; i < kFpsHistorySize; i++) {
                float val = getValue(i);
                if (val > 0.0f) {
                    dataPoints.push_back(toScreen((float)i, val));
                }
            }

            if (dataPoints.Size >= 2) {
                // Catmull-Rom 样条插值生成平滑曲线
                ImVector<ImVec2> smoothPoints;
                const int        kSegmentsPerSpan = 8; // 每两个数据点之间插入的段数
                smoothPoints.reserve(dataPoints.Size * kSegmentsPerSpan);

                for (int i = 0; i < dataPoints.Size - 1; i++) {
                    // 获取控制点 p0, p1, p2, p3
                    ImVec2 p0 = (i > 0) ? dataPoints[i - 1] : dataPoints[i];
                    ImVec2 p1 = dataPoints[i];
                    ImVec2 p2 = dataPoints[i + 1];
                    ImVec2 p3 = (i + 2 < dataPoints.Size) ? dataPoints[i + 2] : dataPoints[i + 1];

                    // Catmull-Rom 插值
                    for (int s = 0; s < kSegmentsPerSpan; s++) {
                        float t  = (float)s / (float)kSegmentsPerSpan;
                        float t2 = t * t;
                        float t3 = t2 * t;

                        // Catmull-Rom 基函数
                        float b0 = -0.5f * t3 + t2 - 0.5f * t;
                        float b1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
                        float b2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
                        float b3 = 0.5f * t3 - 0.5f * t2;

                        ImVec2 pt;
                        pt.x = b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x;
                        pt.y = b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y;
                        smoothPoints.push_back(pt);
                    }
                }
                // 添加最后一个点
                smoothPoints.push_back(dataPoints[dataPoints.Size - 1]);

                drawList->AddPolyline(smoothPoints.Data, smoothPoints.Size, lineColor, ImDrawFlags_None, 2.0f);
            }

            drawList->PopClipRect();

            // Y 轴刻度
            char maxLabel[16], minLabel[16];
            snprintf(maxLabel, sizeof(maxLabel), "%.0f", maxVal);
            snprintf(minLabel, sizeof(minLabel), "%.0f", minVal);
            drawList->AddText(ImVec2(plotPos.x + 3, plotPos.y + 1), axisColor, maxLabel);
            drawList->AddText(ImVec2(plotPos.x + 3, plotPos.y + plotSize.y - 13), axisColor, minLabel);

            ImGui::Dummy(plotSize);

            // Tooltip
            if (ImGui::IsItemHovered()) {
                ImVec2 mousePos = ImGui::GetIO().MousePos;
                float  relX     = (mousePos.x - plotPos.x) / plotSize.x;
                int    idx      = (int)(relX * (float)(kFpsHistorySize - 1) + 0.5f);
                if (idx >= 0 && idx < kFpsHistorySize) {
                    float fpsVal = getValue(idx);
                    ImGui::BeginTooltip();
                    ImGui::Text("%.0f FPS", fpsVal);
                    ImGui::EndTooltip();
                }
            }
            MD3::EndCollapsingHeader();
        }

        // ========== 手部追踪区域（占位符）==========
        if (MD3::BeginCollapsingHeader("Hand Tracking", true)) {
            if (ImGui::BeginTable("TrackerTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Status");
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not Available");

                ImGui::EndTable();
            }

            ImGui::Separator();

            // 动画参数显示
            ImGui::TextDisabled("Animation Values:");
            if (ImGui::BeginTable("AnimTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Scale");
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", animScale_);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Rot X");
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", animRotX_);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Rot Y");
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", animRotY_);

                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::TextDisabled("(Hand tracking not implemented in Diligent backend)");
            MD3::EndCollapsingHeader();
        }

        // ========== 视觉效果区域 ==========
        if (MD3::BeginCollapsingHeader("Visuals")) {
            // 暗色模式切换 - 使用 MD3 Toggle
            if (MD3::Toggle("Dark Mode", &appState_->ui.isDarkMode)) {
                // 应用 MD3 主题样式
                MD3::ApplyImGuiStyle();
            }

            MD3::EndCollapsingHeader();
        }

        // ========== 窗口区域 ==========
        if (MD3::BeginCollapsingHeader("Window")) {
            // 全屏切换按钮 - 使用 MD3 Button
            if (MD3::TonalButton(appState_->window.isFullscreen ? "Exit Fullscreen" : "Enter Fullscreen")) {
                if (hwnd_ != nullptr) {
                    if (!appState_->window.isFullscreen) {
                        // 保存当前窗口位置和大小
                        RECT rect;
                        GetWindowRect(hwnd_, &rect);
                        appState_->window.windowedX = rect.left;
                        appState_->window.windowedY = rect.top;
                        appState_->window.windowedW = rect.right - rect.left;
                        appState_->window.windowedH = rect.bottom - rect.top;

                        // 获取显示器信息
                        HMONITOR    hMonitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi       = {sizeof(mi)};
                        GetMonitorInfoW(hMonitor, &mi);

                        // 设置全屏
                        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                     SWP_FRAMECHANGED);
                        appState_->window.isFullscreen = true;
                    } else {
                        // 恢复窗口模式
                        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                        SetWindowPos(hwnd_, HWND_NOTOPMOST, appState_->window.windowedX, appState_->window.windowedY,
                                     appState_->window.windowedW, appState_->window.windowedH, SWP_FRAMECHANGED);
                        appState_->window.isFullscreen = false;
                    }
                }
            }

            ImGui::Dummy(ImVec2(0, 5));

            // VSync 模式选择 - 使用 MD3 Combo
            ImGui::Text("VSync:");
            const char* vsyncModes[] = {"Off", "On"};
            MD3::Combo("##VSync", &appState_->render.vsyncMode, vsyncModes, 2);

            ImGui::Dummy(ImVec2(0, 5));

            // 显示状态
            ImGui::Text("Fullscreen: %s", appState_->window.isFullscreen ? "Yes" : "No");
            ImGui::Text("Transparent: No");
            MD3::EndCollapsingHeader();
        }

        // ========== 高级区域 ==========
        if (MD3::BeginCollapsingHeader("Advanced")) {
            // Bloom 强度 - 使用 MD3 Slider
            ImGui::Text("Bloom Strength:");
            MD3::Slider("##Bloom", &bloomStrength_, 0.0f, 2.0f, "%.2f");

            ImGui::Dummy(ImVec2(0, 5));

            // 显示一些调试信息
            ImGui::TextDisabled("Debug Info:");
            ImGui::Text("Star Count: %u", starCount_);
            ImGui::Text("Offscreen: %u x %u", surfaceSize_.Width, surfaceSize_.Height);
            MD3::EndCollapsingHeader();
        }

        // ========== LOD 控制区域 ==========
        if (MD3::BeginCollapsingHeader("LOD Control")) {
            // 锁定 LOD 开关
            MD3::Toggle("Lock LOD", &appState_->lod.locked);

            ImGui::Dummy(ImVec2(0, 5));

            // 粒子数量滑块
            ImGui::Text("Particle Count:");
            float particleCount = static_cast<float>(appState_->render.activeParticleCount);
            if (MD3::Slider("##ParticleCount", &particleCount, 1000.0f, 500000.0f, "%.0f")) {
                appState_->render.activeParticleCount = static_cast<uint32_t>(particleCount);
            }

            ImGui::Dummy(ImVec2(0, 5));

            // 像素比例滑块
            ImGui::Text("Pixel Ratio:");
            MD3::Slider("##PixelRatio", &appState_->render.pixelRatio, 0.25f, 2.0f, "%.2f");

            ImGui::Dummy(ImVec2(0, 5));

            // 密度补偿
            ImGui::Text("Density Compensation:");
            MD3::Slider("##DensityComp", &appState_->render.densityComp, 0.0f, 2.0f, "%.2f");

            MD3::EndCollapsingHeader();
        }

        // ========== 日志区域 ==========
        if (MD3::BeginCollapsingHeader("Log")) {
            // 日志面板
            static char searchFilter[128] = "";
            static int  levelFilter       = 0; // 0=All, 1=Info, 2=Warn, 3=Error

            ImGui::Text("Search:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputText("##LogSearch", searchFilter, sizeof(searchFilter));

            const char* levels[] = {"All", "Info", "Warn", "Error"};
            ImGui::Text("Level:");
            ImGui::SameLine();
            MD3::Combo("##LogLevel", &levelFilter, levels, 4);

            ImGui::Dummy(ImVec2(0, 5));

            // 绘制日志
            DebugLog::Instance().Draw(searchFilter, levelFilter);

            if (MD3::TextButton("Clear Log")) {
                DebugLog::Instance().Clear();
            }

            MD3::EndCollapsingHeader();
        }

        // 处理平滑滚动（必须在 WindowScrollbar 之前调用）
        MD3::HandleSmoothScroll(90.0f);

        // 自定义滚动条和缩放手柄
        MD3::WindowScrollbar(kTitleBarHeight);
        MD3::WindowResize(280.0f, 200.0f);

        ImGui::End();

        // MD3 帧结束
        MD3::EndFrame();
    }

    // 先清屏 SwapChain（确保深度缓冲/RT 链路始终一致），再走离屏渲染 + 拷贝。
    // 1. Clear
    RenderClear();

    // 2. Offscreen Rendering (Compute + Stars + Particles)
    RenderOffscreen();

    // 3. Blit to Backbuffer
    BlitOffscreenToBackBuffer();

    // 渲染七段数码管 FPS（在 BlitOffscreenToBackBuffer 之后）
    RenderSevenSegmentFPS();

    // 渲染 ImGui（在七段数码管之后，Present 之前）
    if (imgui_) {
        ITextureView* pBackBufferRTV = swapChain_->GetCurrentBackBufferRTV();
        if (pBackBufferRTV != nullptr) {
            imgui_->Render(immediateContext_, pBackBufferRTV);
        }
    }

    // 7. Present
    immediateContext_->Flush();

    // D3D12: Adaptive VSync (-1) 可能与帧等待对象冲突，使用标准 VSync
    int vsync = appState_->render.vsyncMode;
    if (vsync < 0) {
        vsync = 1;
    }
    swapChain_->Present(static_cast<Uint32>(vsync));
}

void DiligentBackend::RenderSevenSegmentFPS() {
    if (sevenSegPSO_ == nullptr || sevenSegSRB_ == nullptr || sevenSegConstants_ == nullptr) {
        return;
    }

    // 设置渲染目标为 SwapChain BackBuffer
    ITextureView* pRTV = swapChain_->GetCurrentBackBufferRTV();
    immediateContext_->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口
    Viewport vp{};
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    immediateContext_->SetPipelineState(sevenSegPSO_);

    // 计算正交投影矩阵（左下角原点，像素坐标）
    const float L = 0.0f;
    const float R = static_cast<float>(surfaceSize_.Width);
    const float T = static_cast<float>(surfaceSize_.Height);
    const float B = 0.0f;

    // 根据帧率选择颜色（与 OpenGL 版一致）
    float colorR, colorG, colorB;
    if (currentFps_ > 50.0f) {
        colorR = 0.3f;
        colorG = 1.0f;
        colorB = 0.3f; // 绿色
    } else if (currentFps_ > 30.0f) {
        colorR = 1.0f;
        colorG = 0.6f;
        colorB = 0.0f; // 橙色
    } else {
        colorR = 1.0f;
        colorG = 0.2f;
        colorB = 0.2f; // 红色
    }

    // FPS 数字渲染参数（右上角）
    const float numSize      = 20.0f;
    const float digitSpacing = numSize + 10.0f;
    float       xCursor      = static_cast<float>(surfaceSize_.Width) - 60.0f;
    const float yPos         = static_cast<float>(surfaceSize_.Height) - 40.0f;

    // 获取 FPS 数字
    int  displayFps = static_cast<int>(currentFps_);
    char fpsBuffer[8];
    int  fpsLen = snprintf(fpsBuffer, sizeof(fpsBuffer), "%d", displayFps);

    // 从右到左渲染每个数字
    for (int i = fpsLen - 1; i >= 0; --i) {
        int digit = fpsBuffer[i] - '0';
        if (digit < 0 || digit > 9) {
            continue;
        }
        if (sevenSegVB_[digit] == nullptr || sevenSegVertexCount_[digit] == 0) {
            continue;
        }

        // 更新常量缓冲
        struct SevenSegCB {
            float Projection[16];
            float Transform[4]; // x, y, scaleX, scaleY
            float Color[4];     // r, g, b, pad
        };

        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(sevenSegConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            auto* cb = static_cast<SevenSegCB*>(mapped);

            // 正交投影矩阵（列主序）
            // Diligent/D3D 使用列主序，需要转置
            std::memset(cb->Projection, 0, sizeof(cb->Projection));
            cb->Projection[0]  = 2.0f / (R - L);     // [0][0]
            cb->Projection[5]  = 2.0f / (T - B);     // [1][1]
            cb->Projection[10] = 1.0f;               // [2][2]
            cb->Projection[12] = -(R + L) / (R - L); // [3][0]
            cb->Projection[13] = -(T + B) / (T - B); // [3][1]
            cb->Projection[15] = 1.0f;               // [3][3]

            cb->Transform[0] = xCursor;
            cb->Transform[1] = yPos;
            cb->Transform[2] = numSize;
            cb->Transform[3] = numSize;

            cb->Color[0] = colorR;
            cb->Color[1] = colorG;
            cb->Color[2] = colorB;
            cb->Color[3] = 1.0f;

            immediateContext_->UnmapBuffer(sevenSegConstants_, MAP_WRITE);
        }

        immediateContext_->CommitShaderResources(sevenSegSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // 绑定顶点缓冲
        IBuffer*     pBuffs[]  = {sevenSegVB_[digit]};
        const Uint64 offsets[] = {0};
        immediateContext_->SetVertexBuffers(0, 1, pBuffs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        // 绘制
        DrawAttribs draw{};
        draw.NumVertices = sevenSegVertexCount_[digit];
        draw.Flags       = DRAW_FLAG_VERIFY_ALL;
        immediateContext_->Draw(draw);

        xCursor -= digitSpacing;
    }
}

bool DiligentBackend::HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    if (imgui_) {
        return imgui_->HandleWin32Message(hwnd, msg, wParam, lParam);
    }
    return false;
}

} // namespace ParticleSaturn::Render
