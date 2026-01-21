#include "DiligentBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <random>
#include <thread>
#include <vector>

#include "../DebugLog.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "../Settings.h"
#include "../ShaderCache.h"
#include "../ShaderCompileProgress.h"
#include "../generated/LogControlIcons.h"
#include "ArchiverFactoryLoader.h"
#include "CommandQueueD3D12.h"
#include "CrashAnalyzer.h"
#include "DataBlobImpl.hpp"
#include "DeviceContextD3D12.h"
#include "EngineFactoryD3D11.h"
#include "EngineFactoryD3D12.h"
#include "EngineFactoryVk.h"
#include "GraphicsTypes.h"
#include "HandTracker.h"
#include "HandTrackerController.h"
#include "ImGuiDiligent.h"
#include "InputLayout.h"
#include "NativeWindow.h"
#include "RenderDeviceD3D11.h"
#include "RenderDeviceD3D12.h"
#include "Sampler.h"
#include "VulkanD3D12Interop.h"
#include "Win32WindowManager.h"
#include "imgui.h"
#include "md3/MD3.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dwmapi.h>
#include <wrl/client.h> // For Microsoft::WRL::ComPtr

namespace ParticleSaturn::Render {

using namespace Diligent;

// 构造函数和析构函数
DiligentBackend::DiligentBackend() = default;

DiligentBackend::~DiligentBackend() {
    Shutdown();
}

namespace {

static constexpr TEXTURE_FORMAT kOffscreenColorFormat = TEX_FORMAT_R11G11B10_FLOAT;

// Draw/DrawIndirect 验证开关：
// - Debug：启用验证，便于定位资源状态/参数错误
// - Release/FastRelease/Release_Static：全部关闭，避免开发期验证逻辑污染性能
//
// 注意：Diligent 自身在非 Development 构建下也会禁用验证，但这里仍显式置零，
// 保证“Release 系列”完全不走任何额外验证路径。
#if defined(NDEBUG)
static constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_NONE;
#else
static constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_VERIFY_ALL;
#endif

// OpenGL 版星空策略：
// - 基准星数固定为 5 万（STAR_COUNT=50000）
// - 在低 pixelRatio 时绘制数量降到 60%（OpenGL：pixelRatio < 0.85）
static constexpr uint32_t kStarCountBase = 50000u;
static constexpr float    kStarLodRatio  = 0.6f;

static constexpr uint32_t kParticleCountMax = 1200000u;
static constexpr uint32_t kParticleCountMin = 200000u;
static constexpr float    kDensityCompBase  = 0.6f;

static uint32_t HashFNV1a32Append(uint32_t hash, const char* s) {
    if (s == nullptr) {
        return hash;
    }
    while (*s) {
        hash ^= static_cast<uint8_t>(*s++);
        hash *= 16777619u;
    }
    return hash;
}

static const char* GetBackendName(Backend backend) {
    switch (backend) {
    case Backend::D3D11:
        return "d3d11";
    case Backend::D3D12:
        return "d3d12";
    case Backend::Vulkan:
        return "vulkan";
    default:
        return "unknown";
    }
}

static Uint32 ComputeRenderStateCacheContentVersion(Backend backend) {
    // ContentVersion 参与 Diligent RenderStateCache 的版本检查：
    // - 写入：WriteToBlob(ContentVersion)
    // - 读取：Load(blob, ContentVersion)
    //
    // 这里用“应用构建标识 + DiligentCore 版本 + 后端”生成 32-bit hash，
    // 保证任一发生变化时缓存自动失效（避免 ~0u 跳过版本检查导致的缓存污染）。
    uint32_t h = 2166136261u;
    h          = HashFNV1a32Append(h, "ParticleSaturn.Diligent.RenderStateCache|");
    h          = HashFNV1a32Append(h, GetBackendName(backend));
    h          = HashFNV1a32Append(h, "|");
#ifdef APP_BUILD_ID
    h = HashFNV1a32Append(h, APP_BUILD_ID);
#else
    h = HashFNV1a32Append(h, "unknown-build");
#endif
    h = HashFNV1a32Append(h, "|");
#ifdef DILIGENTCORE_COMMIT
    h = HashFNV1a32Append(h, DILIGENTCORE_COMMIT);
#else
    h = HashFNV1a32Append(h, "unknown-diligentcore");
#endif

    if (h == 0) {
        h = 1;
    }
    return static_cast<Uint32>(h);
}

float ComputeDensityComp(uint32_t particleCount, float pixelRatio) {
    const float pr = (pixelRatio > 0.0f) ? pixelRatio : 1.0f;
    const float ratio =
        (kParticleCountMax > 0) ? (static_cast<float>(particleCount) / static_cast<float>(kParticleCountMax)) : 1.0f;
    const float safeRatio = std::max(ratio, 0.0001f);
    return kDensityCompBase / std::pow(safeRatio, 0.7f) / std::pow(pr, 0.5f);
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
                                              const char* source, SHADER_SOURCE_LANGUAGE language,
                                              IRenderStateCache* cache = nullptr) {
    ShaderCreateInfo shaderCI{};
    shaderCI.Desc.Name       = name;
    shaderCI.Desc.ShaderType = type;
    shaderCI.SourceLanguage  = language;
    shaderCI.EntryPoint      = "main";
    shaderCI.Source          = source;

    RefCntAutoPtr<IShader> shader;

    // 优先使用缓存创建着色器
    if (cache != nullptr) {
        cache->CreateShader(shaderCI, &shader);
    }

    // 如果缓存未命中或缓存不可用，直接创建
    if (shader == nullptr) {
        device->CreateShader(shaderCI, &shader);
    }

    return shader;
}

// 通过缓存创建 Graphics PSO 的辅助函数
// 如果缓存可用且命中则返回 true，否则通过 device 创建并返回 false
bool CreateGraphicsPSO(IRenderDevice* device, const GraphicsPipelineStateCreateInfo& psoCI,
                       IPipelineState** ppPSO, IRenderStateCache* cache = nullptr) {
    *ppPSO = nullptr;

    // 优先使用缓存创建 PSO
    if (cache != nullptr) {
        cache->CreateGraphicsPipelineState(psoCI, ppPSO);
    }

    // 如果缓存未命中或缓存不可用，直接创建
    if (*ppPSO == nullptr) {
        device->CreateGraphicsPipelineState(psoCI, ppPSO);
        return false;
    }

    return true;
}

// 通过缓存创建 Compute PSO 的辅助函数
bool CreateComputePSO(IRenderDevice* device, const ComputePipelineStateCreateInfo& psoCI,
                      IPipelineState** ppPSO, IRenderStateCache* cache = nullptr) {
    *ppPSO = nullptr;

    // 优先使用缓存创建 PSO
    if (cache != nullptr) {
        cache->CreateComputePipelineState(psoCI, ppPSO);
    }

    // 如果缓存未命中或缓存不可用，直接创建
    if (*ppPSO == nullptr) {
        device->CreateComputePipelineState(psoCI, ppPSO);
        return false;
    }

    return true;
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

    // VSync 行为：
    // - OpenGL：若支持 Adaptive（-1），默认启用；否则回退为 On（1）。
    // - D3D12：已知 -1 在某些环境可能导致启动即白屏/卡死，故标记为不支持并强制回退到 1。
    // - Vulkan：支持自适应 VSync。Diligent 在 SyncInterval=1 时优先选择 FIFO_RELAXED（自适应），
    //   帧来得及则等 VBlank，帧晚则立即显示（可能撕裂），实现低延迟的帧率限制。
    if (appState_ != nullptr) {
        appState_->render.adaptiveVSyncSupported = (backend_ == Backend::Vulkan);
        if (appState_->render.vsyncMode < 0 && !appState_->render.adaptiveVSyncSupported) {
            appState_->render.vsyncMode = 1;
        }
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
        // 增加 GPU 描述符堆大小，避免动态描述符耗尽
        engineCI.GPUDescriptorHeapDynamicSize[0] = 32768; // CBV_SRV_UAV
        engineCI.GPUDescriptorHeapDynamicSize[1] = 2048;  // SAMPLER
        factory->CreateDeviceAndContextsD3D12(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"D3D12 设备或上下文创建失败。");
            return false;
        }

        // 检查是否需要透明模式（Mica/Acrylic 需要 DirectComposition SwapChain）
        const bool needTransparent = (appState_ != nullptr && appState_->backdrop.useTransparent);

        if (needTransparent) {
            // 使用 DirectComposition SwapChain 实现透明窗口
            std::cout << "[DiligentBackend] Transparent mode enabled, using DirectComposition SwapChain" << std::endl;

            // 获取 D3D12 设备和命令队列
            RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
            device_->QueryInterface(IID_RenderDeviceD3D12,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
            if (!deviceD3D12) {
                SetLastError(L"无法获取 IRenderDeviceD3D12 接口。");
                return false;
            }

            ID3D12Device* d3d12Device = deviceD3D12->GetD3D12Device();

            // 获取命令队列（通过 LockCommandQueue + ICommandQueueD3D12 接口）
            ICommandQueue* cmdQueueBase = immediateContext_->LockCommandQueue();
            if (!cmdQueueBase) {
                SetLastError(L"无法锁定命令队列。");
                return false;
            }

            RefCntAutoPtr<ICommandQueueD3D12> cmdQueueD3D12;
            cmdQueueBase->QueryInterface(
                IID_CommandQueueD3D12, reinterpret_cast<IObject**>(static_cast<ICommandQueueD3D12**>(&cmdQueueD3D12)));
            if (!cmdQueueD3D12) {
                immediateContext_->UnlockCommandQueue();
                SetLastError(L"无法获取 ICommandQueueD3D12 接口。");
                return false;
            }

            ID3D12CommandQueue* cmdQueue = cmdQueueD3D12->GetD3D12CommandQueue();

            // 初始化 DirectComposition SwapChain（D3D12 版本）
            if (!dcompSwapChain_.InitD3D12(hwnd, d3d12Device, cmdQueue, initialSize.Width, initialSize.Height, 3)) {
                immediateContext_->UnlockCommandQueue();
                SetLastError(L"DirectComposition SwapChain 初始化失败。");
                return false;
            }

            immediateContext_->UnlockCommandQueue();

            // 创建 Diligent 后缓冲 RTV
            if (!CreateDCompBackBufferRTVs()) {
                SetLastError(L"创建 DirectComposition 后缓冲 RTV 失败。");
                return false;
            }

            useDCompSwapChain_ = true;
            surfaceSize_       = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
        } else {
            // 非透明模式：使用 Diligent 标准 SwapChain
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            // 诊断：打印实际使用的颜色格式（5=SRGB, 4=UNORM）
            std::cout << "[DiligentBackend] D3D12 SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    } else if (backend == Backend::Vulkan) {
        auto* factory = GetEngineFactoryVk();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryVk() 返回空。");
            return false;
        }

        EngineVkCreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif

        // 如果需要透明模式，添加外部内存扩展以支持 D3D12-Vulkan 互操作
        // 回退：禁用 Vulkan 透明模式，强制使用标准 SwapChain
        const bool needTransparent = false; // (appState_ != nullptr && appState_->backdrop.useTransparent);
        OutputDebugStringA(needTransparent ? "[DiligentBackend] needTransparent = TRUE\n"
                                           : "[DiligentBackend] needTransparent = FALSE (Vulkan interop disabled)\n");
        std::vector<const char*> deviceExtensions;
        if (needTransparent) {
            deviceExtensions.push_back("VK_KHR_external_memory_win32");
            deviceExtensions.push_back("VK_KHR_external_semaphore_win32");
            engineCI.DeviceExtensionCount   = static_cast<Uint32>(deviceExtensions.size());
            engineCI.ppDeviceExtensionNames = deviceExtensions.data();
            OutputDebugStringA("[DiligentBackend] Vulkan transparent mode: enabling external memory extensions\n");
        }

        factory->CreateDeviceAndContextsVk(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"Vulkan 设备或上下文创建失败。");
            return false;
        }

        if (needTransparent) {
            vkD3D12Interop_ = std::make_unique<VulkanD3D12Interop>();
            if (!vkD3D12Interop_->Init(hwnd, device_, immediateContext_, scDesc.Width, scDesc.Height)) {
                std::cerr << "[DiligentBackend] Failed to init VulkanD3D12Interop" << std::endl;
                SetLastError(L"Vulkan Interop 初始化失败。");
                vkD3D12Interop_.reset();
                return false;
            }
            useVkD3D12Interop_ = true;
            surfaceSize_       = {vkD3D12Interop_->GetWidth(), vkD3D12Interop_->GetHeight()};
        } else {
            factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            useVkD3D12Interop_      = false;
            // 诊断：打印实际使用的颜色格式（5=SRGB, 4=UNORM）
            std::cout << "[DiligentBackend] Vulkan SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    } else if (backend == Backend::D3D11) {
        // D3D11 初始化
        auto* factory = GetEngineFactoryD3D11();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryD3D11() 返回空。");
            return false;
        }

        EngineD3D11CreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif

        factory->CreateDeviceAndContextsD3D11(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"D3D11 设备或上下文创建失败。");
            return false;
        }

        // 检查是否需要透明模式（Mica/Acrylic 需要 DirectComposition SwapChain）
        const bool needTransparent = (appState_ != nullptr && appState_->backdrop.useTransparent);

        if (needTransparent) {
            // 使用 DirectComposition SwapChain 实现透明窗口
            std::cout << "[DiligentBackend] D3D11 Transparent mode enabled, using DirectComposition SwapChain"
                      << std::endl;

            // 获取 D3D11 设备
            RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
            device_->QueryInterface(IID_RenderDeviceD3D11,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
            if (!deviceD3D11) {
                SetLastError(L"无法获取 IRenderDeviceD3D11 接口。");
                return false;
            }

            ID3D11Device* d3d11Device = deviceD3D11->GetD3D11Device();

            // 初始化 DirectComposition SwapChain（D3D11 版本）
            if (!dcompSwapChain_.InitD3D11(hwnd, d3d11Device, initialSize.Width, initialSize.Height, 3)) {
                SetLastError(L"D3D11 DirectComposition SwapChain 初始化失败。");
                return false;
            }

            // 创建 Diligent 后缓冲 RTV
            if (!CreateDCompBackBufferRTVs()) {
                SetLastError(L"创建 D3D11 DirectComposition 后缓冲 RTV 失败。");
                return false;
            }

            useDCompSwapChain_ = true;
            surfaceSize_       = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
        } else {
            // 非透明模式：使用 Diligent 标准 SwapChain
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D11(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"D3D11 SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            // 诊断：打印实际使用的颜色格式
            std::cout << "[DiligentBackend] D3D11 SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    }

    // 注意：Win32 的 WM_SIZE/ClientRect 尺寸在某些 DPI/缩放配置下可能与 SwapChain 实际尺寸不完全一致。
    // 后续渲染/点精灵的像素尺寸换算依赖"真实 RT 尺寸"，这里 surfaceSize_ 已在上面设置。
    startTime_    = std::chrono::steady_clock::now();
    lastAnimTime_ = std::chrono::steady_clock::time_point{};
    animAutoTime_ = 0.0f;
    animScale_    = 1.0f;
    animRotX_     = 0.4f;
    animRotY_     = 0.0f;

    // 初始化着色器缓存
    renderStateCacheContentVersion_ = ComputeRenderStateCacheContentVersion(backend_);
    std::cerr << "[DiligentBackend] RenderStateCache ContentVersion (expected) = " << renderStateCacheContentVersion_
              << " (0x" << std::hex << renderStateCacheContentVersion_ << std::dec << ")" << std::endl;

    std::cerr << "[DiligentBackend] Initializing RenderStateCache..." << std::endl;
    bool cacheLoadedOk = false;
    {
        IArchiverFactory* archiverFactory = Diligent::GetArchiverFactory();
        if (archiverFactory != nullptr) {
            RenderStateCacheCreateInfo cacheCI{};
            cacheCI.pDevice          = device_;
            cacheCI.pArchiverFactory = archiverFactory;
            cacheCI.LogLevel         = RENDER_STATE_CACHE_LOG_LEVEL_NORMAL;
            cacheCI.EnableHotReload  = false;

            CreateRenderStateCache(cacheCI, &renderStateCache_);

            if (renderStateCache_) {
                // 尝试加载现有缓存
                const char* backendName = (backend_ == Backend::D3D11)  ? "d3d11"
                                          : (backend_ == Backend::D3D12) ? "d3d12"
                                                                         : "vulkan";
                auto        cachePath   = ShaderCache::GetDiligentCachePath(backendName);
                if (!cachePath.empty()) {
                    std::vector<uint8_t> cacheData;
                    if (ShaderCache::ReadCache(cachePath, cacheData)) {
                        std::cerr << "[DiligentBackend] Read cache data: " << cacheData.size() << " bytes" << std::endl;
                        auto dataBlob = Diligent::DataBlobImpl::Create(cacheData.size(), cacheData.data());
                        if (dataBlob && renderStateCache_->Load(dataBlob, renderStateCacheContentVersion_, false)) {
                            std::cerr << "[DiligentBackend] RenderStateCache loaded from disk" << std::endl;
                            cacheLoadedOk = true;
                        } else {
                            std::cerr << "[DiligentBackend] RenderStateCache Load() failed" << std::endl;
                            // 避免每次启动都反复尝试加载同一个坏缓存
                            ShaderCache::InvalidateCache(cachePath);
                        }
                    } else {
                        std::cerr << "[DiligentBackend] No cache file found or version mismatch" << std::endl;
                    }
                }
            } else {
                std::cerr << "[DiligentBackend] CreateRenderStateCache() failed" << std::endl;
            }
        } else {
            std::cerr << "[DiligentBackend] ArchiverFactory not available, shader caching disabled" << std::endl;
        }
    }

    // 检测是否需要编译着色器（缓存未命中时）
    const bool needsCompile = !cacheLoadedOk;

    // 提前初始化 ImGui（用于显示编译进度条）
    hwnd_  = hwnd;
    imgui_ = std::make_unique<UI::ImGuiDiligent>();
    bool imguiOk = false;
    if (useDCompSwapChain_) {
        imguiOk = imgui_->Init(hwnd, backend, device_, TEX_FORMAT_RGBA8_UNORM, surfaceSize_.Width, surfaceSize_.Height);
    } else if (useVkD3D12Interop_ && vkD3D12Interop_) {
        imguiOk = imgui_->Init(hwnd, backend, device_, TEX_FORMAT_RGBA8_UNORM, surfaceSize_.Width, surfaceSize_.Height);
    } else {
        imguiOk = imgui_->Init(hwnd, backend, device_, swapChain_);
    }
    if (!imguiOk) {
        if (lastError_.empty()) {
            SetLastError(L"ImGui 初始化失败。");
        }
        return false;
    }

    // 进度条状态
    ShaderCompileProgress::ProgressRenderer progressRenderer;
    const int kTotalPSOSteps = 7; // FullscreenQuad, Bloom, Acrylic, Starfield, Particle, ParticleCompute, SevenSegment
    progressRenderer.SetTotal(kTotalPSOSteps);
    const bool isDarkMode = Win32WindowManager::IsSystemDarkMode();
    auto lastFrameTime = std::chrono::steady_clock::now();

    // 进度条渲染辅助 lambda
    auto renderProgress = [&]() {
        if (!needsCompile) return; // 缓存命中时不显示进度条

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        // 开始 ImGui 帧
        imgui_->NewFrame();
        ImGui::NewFrame();

        // 渲染进度条
        progressRenderer.Render(isDarkMode, dt);

        // 结束 ImGui 帧
        ImGui::Render();

        // 获取 RTV 并渲染
        ITextureView* pRTV = GetCurrentBackBufferRTV();
        if (pRTV) {
            imgui_->Render(immediateContext_, pRTV);
        }

        // Present
        PresentFrame(0);
    };

    // 显示初始进度
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating FullscreenQuadPSO...\n");
    if (!CreateFullscreenQuadPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateFullscreenQuadPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateFullscreenQuadPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] FullscreenQuadPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating OffscreenRenderTarget...\n");
    if (!CreateOffscreenRenderTarget(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateOffscreenRenderTarget FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateOffscreenRenderTarget() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] OffscreenRenderTarget OK\n");
    UpdateFullscreenQuadBindings();

    // Bloom / Blur 资源（用于 Bloom 合成与 UI 玻璃模糊）
    OutputDebugStringA("[DiligentBackend] Creating BloomPSO...\n");
    if (!CreateBloomPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateBloomPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateBloomPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] BloomPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating AcrylicPSO...\n");
    if (!CreateAcrylicPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateAcrylicPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateAcrylicPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] AcrylicPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();;

    OutputDebugStringA("[DiligentBackend] Creating BloomTextures...\n");
    if (!CreateBloomTextures(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateBloomTextures FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateBloomTextures() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] BloomTextures OK\n");

    OutputDebugStringA("[DiligentBackend] Creating UISceneTextures...\n");
    if (!CreateUISceneTextures(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateUISceneTextures FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateUISceneTextures() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] UISceneTextures OK\n");

    // 阶段 2：星空（先用 2D NDC 点列表验证 point 渲染 + 闪烁 + 混合链路）。
    // 对齐 OpenGL：基准星数固定为 5 万，LOD 仅在 Draw 时按 pixelRatio 调整绘制数量。
    if (!CreateStarfieldBuffers(kStarCountBase)) {
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
    progressRenderer.IncrementCompleted();
    renderProgress();

    // 阶段 3（第 1 步）：粒子数据通路（先 CPU 复刻初始化，后续再接 compute 三缓冲轮转）。
    // 复刻 OpenGL 旧版默认粒子规模：120 万（视觉遮蔽/密度/"不透光感"强相关）。
    if (!CreateParticleBuffers(kParticleCountMax)) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleBuffers() 失败。");
        }
        return false;
    }
    if (appState_ != nullptr) {
        if (appState_->render.activeParticleCount == 0) {
            appState_->render.activeParticleCount = particleCount_;
        }
        if (appState_->render.pixelRatio <= 0.0f) {
            appState_->render.pixelRatio = 1.0f;
        }
        if (!lastLodBasisValid_) {
            lastLodParticleCount_ = appState_->render.activeParticleCount;
            lastLodPixelRatio_    = appState_->render.pixelRatio;
            lastLodBasisValid_    = true;
        }
        // 初始密度补偿：与 OpenGL 旧公式一致
        appState_->render.densityComp =
            ComputeDensityComp(appState_->render.activeParticleCount, appState_->render.pixelRatio);
    }
    if (!CreateParticlePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticlePSO() 失败。");
        }
        return false;
    }
    progressRenderer.IncrementCompleted();
    renderProgress();

    if (!CreateParticleComputePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleComputePSO() 失败。");
        }
        return false;
    }
    progressRenderer.IncrementCompleted();
    renderProgress();

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
    progressRenderer.IncrementCompleted();
    renderProgress();

    // 阶段 6：MD3 UI 系统初始化（使用 AppState 中的 DPI 缩放）
    const float dpiScale = appState_ ? appState_->ui.dpiScale : 1.0f;
    MD3::Init(device_, immediateContext_, backend_, dpiScale);
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));
    MD3::ApplyImGuiStyle();

    // 从注册表加载 ImGui 布局（在第一次 NewFrame 之前）
    Settings::LoadImGuiLayout();

    // 如果启动时透明模式已开启，将辉光设为 0（保留默认值在 bloomStrengthBeforeTransp_ 中）
    if (appState_ != nullptr && appState_->backdrop.useTransparent) {
        bloomStrength_ = 0.0f;
        // bloomStrengthBeforeTransp_ 保持默认值 0.5f，用于关闭透明时恢复
    }

    // HandTracker：启动时与 OpenGL 版一致，弹出（必要时）摄像头选择，并异步初始化追踪器。
    // 模型采用"随可执行文件分发"的策略：构建时会把 HandTracker/models 下的两份 tflite 复制到 exe 同目录。
    handTracker_ = std::make_unique<HandTracking::Controller>();
    if (handTracker_->Init(hwnd, appState_)) {
        // 非阻塞启动：后续在 RenderFrame() 的 Tick() 中轮询 IsTrackerReady()。
        handTracker_->StartWithCameraSelector(false);
    }

    return true;
}

void DiligentBackend::Shutdown() {
    // 保存会话状态到注册表（在释放资源之前）
    if (appState_ != nullptr) {
        Settings::SaveSession(*appState_, backend_);
    }

    // 保存着色器缓存到磁盘
    // 写入日志文件（因为此时 ImGui 已关闭，std::cerr 无法显示）
    auto logPath = ShaderCache::GetCacheDirectory() + L"\\shutdown_log.txt";
    std::wofstream logFile(logPath, std::ios::app);  // 追加模式
    logFile << L"[DiligentBackend] Shutdown called..." << std::endl;

    if (renderStateCache_) {
        const char* backendName = (backend_ == Backend::D3D11)  ? "d3d11"
                                  : (backend_ == Backend::D3D12) ? "d3d12"
                                                                 : "vulkan";
        auto        cachePath   = ShaderCache::GetDiligentCachePath(backendName);
        logFile << L"[DiligentBackend] Cache path: " << cachePath << std::endl;
        if (!cachePath.empty()) {
            RefCntAutoPtr<IDataBlob> cacheBlob;
            // 使用固定 ContentVersion（由构建标识 + DiligentCore 版本 + 后端生成），避免跨版本缓存污染。
            const Uint32 cv = (renderStateCacheContentVersion_ != 0) ? renderStateCacheContentVersion_
                                                                     : ComputeRenderStateCacheContentVersion(backend_);
            bool writeResult = renderStateCache_->WriteToBlob(cv, &cacheBlob);
            logFile << L"[DiligentBackend] WriteToBlob returned: " << (writeResult ? L"true" : L"false") << std::endl;
            if (writeResult && cacheBlob) {
                logFile << L"[DiligentBackend] WriteToBlob succeeded, size: " << cacheBlob->GetSize() << L" bytes" << std::endl;
                if (ShaderCache::WriteCache(cachePath, cacheBlob->GetConstDataPtr(), cacheBlob->GetSize())) {
                    logFile << L"[DiligentBackend] RenderStateCache saved to disk" << std::endl;
                } else {
                    logFile << L"[DiligentBackend] WriteCache() failed, GetLastError=" << GetLastError() << std::endl;
                }
            } else {
                logFile << L"[DiligentBackend] WriteToBlob() failed" << std::endl;
            }
        } else {
            logFile << L"[DiligentBackend] Cache path is empty!" << std::endl;
        }
        renderStateCache_.Release();
    } else {
        logFile << L"[DiligentBackend] renderStateCache_ is null, nothing to save" << std::endl;
    }
    logFile.close();

    if (handTracker_) {
        handTracker_->Shutdown();
        handTracker_.reset();
    }

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

    bloomSRV_B_.Release();
    bloomRTV_B_.Release();
    bloomTexB_.Release();
    bloomSRV_A_.Release();
    bloomRTV_A_.Release();
    bloomTexA_.Release();
    bloomSRV_D_.Release();
    bloomRTV_D_.Release();
    bloomTexD_.Release();
    bloomSRV_C_.Release();
    bloomRTV_C_.Release();
    bloomTexC_.Release();
    bloomBlurConstants_.Release();
    bloomBlurSRB_.Release();
    bloomBlurPSO_.Release();
    bloomDownsampleSRB_.Release();
    bloomDownsamplePSO_.Release();
    bloomConstants_.Release();
    bloomW_  = 0;
    bloomH_  = 0;
    bloomW2_ = 0;
    bloomH2_ = 0;

    acrylicSRB_.Release();
    acrylicPSO_.Release();
    acrylicConstants_.Release();

    uiSceneSRV_.Release();
    uiSceneRTV_.Release();
    uiSceneColor_.Release();

    uiAcrylicSRV_Strong_.Release();
    uiAcrylicRTV_Strong_.Release();
    uiAcrylicStrong_.Release();
    uiAcrylicSRV_Weak_.Release();
    uiAcrylicRTV_Weak_.Release();
    uiAcrylicWeak_.Release();

    uiNoiseSRV_.Release();
    uiNoiseTex_.Release();

    logPauseIconSRV_.Release();
    logPauseIconTex_.Release();
    logResumeIconSRV_.Release();
    logResumeIconTex_.Release();

    uiBlurSRV_D_.Release();
    uiBlurRTV_D_.Release();
    uiBlurTexD_.Release();
    uiBlurSRV_C_.Release();
    uiBlurRTV_C_.Release();
    uiBlurTexC_.Release();
    uiBlurSRV_B_.Release();
    uiBlurRTV_B_.Release();
    uiBlurTexB_.Release();
    uiBlurSRV_A_.Release();
    uiBlurRTV_A_.Release();
    uiBlurTexA_.Release();
    uiBlurW_  = 0;
    uiBlurH_  = 0;
    uiBlurW2_ = 0;
    uiBlurH2_ = 0;

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

    // 在释放 Vulkan 设备之前，先清理 VulkanD3D12Interop（它持有 native Vulkan 资源）
    if (vkD3D12Interop_) {
        vkD3D12Interop_->Shutdown();
        vkD3D12Interop_.reset();
    }
    useVkD3D12Interop_ = false;

    swapChain_.Release();
    immediateContext_.Release();
    device_.Release();
}

void DiligentBackend::ClearShaderCache() {
    // 删除当前后端的缓存文件
    const char* backendName = (backend_ == Backend::D3D11)  ? "d3d11"
                              : (backend_ == Backend::D3D12) ? "d3d12"
                                                             : "vulkan";
    auto cachePath = ShaderCache::GetDiligentCachePath(backendName);
    if (!cachePath.empty()) {
        ShaderCache::InvalidateCache(cachePath);
        OutputDebugStringA("[DiligentBackend] Shader cache cleared\n");
    }
}

void DiligentBackend::Resize(SurfaceSize newSize) {
    if (!IsInitialized()) {
        return;
    }
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }

    // 检查尺寸是否变化
    if (surfaceSize_.Width == newSize.Width && surfaceSize_.Height == newSize.Height) {
        return;
    }

    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        // DXGI ResizeBuffers 要求：必须先释放所有对旧 backbuffer 的引用，否则会返回 DXGI_ERROR_INVALID_CALL。
        // 除了 dcompSwapChain_ 内部缓存的原生 backbuffer，这里还持有 Diligent 侧包装后的纹理/RTV 引用，
        // 并且 IDeviceContext 也可能缓存“当前渲染目标”引用。
        if (immediateContext_) {
            immediateContext_->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
            immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                SET_VERTEX_BUFFERS_FLAG_RESET);
            immediateContext_->SetIndexBuffer(nullptr, 0, RESOURCE_STATE_TRANSITION_MODE_NONE);
            immediateContext_->Flush();
        }

        for (auto& rtv : dcompBackBufferRTVs_) {
            rtv.Release();
        }
        for (auto& buf : dcompBackBuffers_) {
            buf.Release();
        }

        // Diligent 在 D3D12 下会延迟释放底层对象（跨帧回收）；仅 Release() 指针可能不足以让 DXGI 看到引用数归零。
        // 这里强制 GPU 空闲并回收 stale resources，尽量保证 ResizeBuffers 一次成功。
        if (device_) {
            device_->IdleGPU();
            device_->ReleaseStaleResources(true);
        }

        // DirectComposition SwapChain 模式
        if (!dcompSwapChain_.Resize(newSize.Width, newSize.Height)) {
            std::cerr << "[DiligentBackend] DComp SwapChain resize failed" << std::endl;
            // ResizeBuffers 失败时必须恢复 backbuffer RTV，否则后续帧会因为 RTV 为空而“卡死”。
            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to restore DComp back buffer RTVs after resize failure"
                          << std::endl;
            }
            return;
        }

        // 重新创建后缓冲 RTV
        if (!CreateDCompBackBufferRTVs()) {
            std::cerr << "[DiligentBackend] Failed to recreate DComp back buffer RTVs after resize" << std::endl;
            return;
        }

        surfaceSize_ = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
    } else if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        // Vulkan 互操作模式
        if (!vkD3D12Interop_->Resize(newSize.Width, newSize.Height)) {
            std::cerr << "[DiligentBackend] Vulkan Interop resize failed" << std::endl;
            return;
        }
        surfaceSize_ = {vkD3D12Interop_->GetWidth(), vkD3D12Interop_->GetHeight()};
    } else if (swapChain_) {
        // 标准 Diligent SwapChain 模式
        swapChain_->Resize(newSize.Width, newSize.Height);
        const auto& newDesc = swapChain_->GetDesc();
        surfaceSize_        = {newDesc.Width, newDesc.Height};
    }

    // SwapChain Resize 只影响后备缓冲/深度缓冲；离屏 RT 需要手动重建。
    CreateOffscreenRenderTarget(surfaceSize_);
    UpdateFullscreenQuadBindings();
    CreateBloomTextures(surfaceSize_);
    CreateUISceneTextures(surfaceSize_);

    // 更新 MD3 屏幕尺寸
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));
}

void DiligentBackend::RequestResize(SurfaceSize newSize) {
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }
    pendingResize_    = newSize;
    hasPendingResize_ = true;
}

bool DiligentBackend::SetBackdropMode(int mode) {
    if (appState_ == nullptr || hwnd_ == nullptr) {
        return false;
    }

    // mode: 0=Solid, 1=Aero, 2=Acrylic, 3=Mica
    const bool wantTransparent = (mode != 0);

    const bool canUseDComp =
        (appState_->backdrop.transparentSupported) && (backend_ == Backend::D3D12 || backend_ == Backend::D3D11);

    // 与 Debug UI 的逻辑保持一致：透明开启时抑制 bloom，关闭时恢复。
    // 注意：这里的“透明”指的是输出 alpha 参与 DWM 合成（Backdrop 开启），与是否实际销毁/重建 SwapChain 解耦。
    const bool wasTransparent = (appState_->backdrop.useTransparent);
    if (wasTransparent != wantTransparent) {
        if (wantTransparent) {
            bloomStrengthBeforeTransp_ = bloomStrength_;
            bloomStrength_             = 0.0f;
        } else {
            bloomStrength_ = bloomStrengthBeforeTransp_;
        }
    }

    if (canUseDComp) {
        // 只在“需要透明但当前没有 DComp”时切换到 DComp。
        // 一旦进入 DComp 模式，不再切回普通 HWND SwapChain：
        // 在 Win11 的部分环境下，运行期反复销毁/重建 DComp SwapChain 会导致系统 Backdrop（Mica/Acrylic）后续再开启失效，
        // 表现为：DWM 退化为纯色背景（用户侧观感：模糊彻底没了）。
        if (wantTransparent && !useDCompSwapChain_) {
            if (!SwitchTransparentMode(true)) {
                return false;
            }
        }
    }

    // 无论是否切换 SwapChain，都同步 DWM Backdrop（并更新 appState_->backdrop.useTransparent）
    std::cout << "[DiligentBackend] SetBackdropMode: backend="
              << (backend_ == Backend::D3D11   ? "D3D11"
                  : backend_ == Backend::D3D12 ? "D3D12"
                   : backend_ == Backend::Vulkan ? "Vulkan" : "Unknown")
              << ", mode=" << mode << ", wantTransparent=" << (wantTransparent ? "true" : "false")
              << ", useDCompSwapChain_=" << (useDCompSwapChain_ ? "true" : "false") << std::endl;
    ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd_, mode, *appState_);
    return true;
}

bool DiligentBackend::CreateFullscreenQuadPSO() {
    if (device_ == nullptr || !IsInitialized()) {
        return false;
    }

    const auto sources = GetFullscreenQuadShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "FullscreenQuad VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language, renderStateCache_);
    const auto ps =
        CreateShaderFromSource(device_, "FullscreenQuad PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language, renderStateCache_);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "FullscreenQuad PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    // 根据当前模式确定 RTV 格式
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (useVkD3D12Interop_ && vkD3D12Interop_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }

    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = rtvFormat;
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

    // Vulkan/GLSL 使用组合采样器 (sampler2D)，采样器名称直接用纹理名
    // D3D12/HLSL 使用分离采样器，采样器名称需要 "_sampler" 后缀
    const char* texSamplerName   = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";
    const char* bloomSamplerName = (backend_ == Backend::Vulkan) ? "g_BloomTexture" : "g_BloomTexture_sampler";

    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, texSamplerName, sampDesc},
        {SHADER_TYPE_PIXEL, bloomSamplerName, sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    fullscreenQuadPSO_.Release();
    fullscreenQuadSRB_.Release();
    CreateGraphicsPSO(device_, psoCI, &fullscreenQuadPSO_, renderStateCache_);
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

bool DiligentBackend::CreateBloomPSO() {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }

    const auto downSources = GetBloomDownsampleShaderSources(backend_);
    const auto blurSources = GetBloomBlurShaderSources(backend_);
    if (downSources.Vertex == nullptr || downSources.Fragment == nullptr || blurSources.Vertex == nullptr ||
        blurSources.Fragment == nullptr) {
        return false;
    }

    const auto downVS = CreateShaderFromSource(device_, "BloomDownsample VS", SHADER_TYPE_VERTEX, downSources.Vertex,
                                               downSources.Language, renderStateCache_);
    const auto downPS = CreateShaderFromSource(device_, "BloomDownsample PS", SHADER_TYPE_PIXEL, downSources.Fragment,
                                               downSources.Language, renderStateCache_);
    const auto blurVS =
        CreateShaderFromSource(device_, "BloomBlur VS", SHADER_TYPE_VERTEX, blurSources.Vertex, blurSources.Language, renderStateCache_);
    const auto blurPS =
        CreateShaderFromSource(device_, "BloomBlur PS", SHADER_TYPE_PIXEL, blurSources.Fragment, blurSources.Language, renderStateCache_);
    if (downVS == nullptr || downPS == nullptr || blurVS == nullptr || blurPS == nullptr) {
        return false;
    }

    // 常量缓冲（BlurCB）：float2 texelSize + float offset + float threshold
    if (bloomBlurConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Bloom Blur Constants";
        cbDesc.Size           = 16;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
        device_->CreateBuffer(cbDesc, nullptr, &bloomBlurConstants_);
        if (bloomBlurConstants_ == nullptr) {
            return false;
        }
    }

    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW  = TEXTURE_ADDRESS_CLAMP;

    // Vulkan/GLSL 使用组合采样器 (sampler2D)，采样器名称直接用纹理名 "g_Texture"
    // D3D12/HLSL 使用分离采样器，采样器名称需要 "_sampler" 后缀
    const char* samplerName = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";

    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, samplerName, sampDesc},
    };

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "BlurCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };

    auto createPso = [&](const char* name, IShader* vs, IShader* ps, RefCntAutoPtr<IPipelineState>& outPso,
                         RefCntAutoPtr<IShaderResourceBinding>& outSrb) -> bool {
        GraphicsPipelineStateCreateInfo psoCI{};
        psoCI.PSODesc.Name         = name;
        psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

        // 输出到 Bloom 纹理（R11G11B10F），不需要 DSV
        psoCI.GraphicsPipeline.NumRenderTargets = 1;
        psoCI.GraphicsPipeline.RTVFormats[0]    = kOffscreenColorFormat;
        psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN;

        psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

        psoCI.PSODesc.ResourceLayout.NumVariables         = _countof(vars);
        psoCI.PSODesc.ResourceLayout.Variables            = vars;
        psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
        psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

        psoCI.pVS = vs;
        psoCI.pPS = ps;

        outPso.Release();
        outSrb.Release();
        CreateGraphicsPSO(device_, psoCI, &outPso, renderStateCache_);
        if (outPso == nullptr) {
            return false;
        }

        if (auto* cbVar = outPso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BlurCB"); cbVar != nullptr) {
            cbVar->Set(bloomBlurConstants_);
        } else {
            return false;
        }

        outPso->CreateShaderResourceBinding(&outSrb, true);
        return outSrb != nullptr;
    };

    if (!createPso("Bloom Downsample PSO", downVS, downPS, bloomDownsamplePSO_, bloomDownsampleSRB_)) {
        return false;
    }
    if (!createPso("Bloom Blur PSO", blurVS, blurPS, bloomBlurPSO_, bloomBlurSRB_)) {
        return false;
    }

    return true;
}

bool DiligentBackend::CreateAcrylicPSO() {
    if (device_ == nullptr) {
        return false;
    }

    const auto sources = GetAcrylicCompositeShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "AcrylicComposite VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language, renderStateCache_);
    const auto ps =
        CreateShaderFromSource(device_, "AcrylicComposite PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language, renderStateCache_);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    // 常量缓冲：2个 float4（tint + params）
    if (acrylicConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Acrylic Constants";
        cbDesc.Size           = 32;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
        device_->CreateBuffer(cbDesc, nullptr, &acrylicConstants_);
        if (acrylicConstants_ == nullptr) {
            return false;
        }
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "AcrylicComposite PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN;

    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "AcrylicCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
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

    const char*                samplerName     = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";
    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, samplerName, sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    acrylicPSO_.Release();
    acrylicSRB_.Release();
    CreateGraphicsPSO(device_, psoCI, &acrylicPSO_, renderStateCache_);
    if (acrylicPSO_ == nullptr) {
        return false;
    }

    if (auto* var = acrylicPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "AcrylicCB"); var != nullptr) {
        var->Set(acrylicConstants_);
    }

    acrylicPSO_->CreateShaderResourceBinding(&acrylicSRB_, true);
    return acrylicSRB_ != nullptr;
}

bool DiligentBackend::CreateBloomTextures(SurfaceSize size) {
    if (device_ == nullptr) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return true;
    }

    // 对齐 OpenGL 的玻璃模糊分辨率：1/6
    const uint32_t w = std::max(1u, size.Width / 6u);
    const uint32_t h = std::max(1u, size.Height / 6u);

    // 次级模糊分辨率：1/12（用于折叠区域 Acrylic 效果）
    const uint32_t w2 = std::max(1u, size.Width / 12u);
    const uint32_t h2 = std::max(1u, size.Height / 12u);

    const bool sizeChanged = (bloomW_ != w || bloomH_ != h || bloomW2_ != w2 || bloomH2_ != h2);
    if (!sizeChanged && bloomTexA_ != nullptr && bloomTexB_ != nullptr && bloomTexC_ != nullptr &&
        bloomTexD_ != nullptr) {
        return true;
    }

    bloomW_  = w;
    bloomH_  = h;
    bloomW2_ = w2;
    bloomH2_ = h2;

    auto createTex = [&](const char* name, uint32_t texW, uint32_t texH, RefCntAutoPtr<ITexture>& outTex,
                         RefCntAutoPtr<ITextureView>& outRTV, RefCntAutoPtr<ITextureView>& outSRV) -> bool {
        TextureDesc texDesc{};
        texDesc.Name      = name;
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = texW;
        texDesc.Height    = texH;
        texDesc.MipLevels = 1;
        texDesc.Format    = kOffscreenColorFormat;
        texDesc.Usage     = USAGE_DEFAULT;
        texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;

        outTex.Release();
        outRTV.Release();
        outSRV.Release();

        device_->CreateTexture(texDesc, nullptr, &outTex);
        if (outTex == nullptr) {
            return false;
        }
        outRTV = outTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        outSRV = outTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        return outRTV != nullptr && outSRV != nullptr;
    };

    // 1/6 分辨率纹理（窗口背景强模糊）
    if (!createTex("Bloom Tex A", bloomW_, bloomH_, bloomTexA_, bloomRTV_A_, bloomSRV_A_)) {
        return false;
    }
    if (!createTex("Bloom Tex B", bloomW_, bloomH_, bloomTexB_, bloomRTV_B_, bloomSRV_B_)) {
        return false;
    }

    // 1/12 分辨率纹理（折叠区域弱模糊）
    if (!createTex("Bloom Tex C", bloomW2_, bloomH2_, bloomTexC_, bloomRTV_C_, bloomSRV_C_)) {
        return false;
    }
    if (!createTex("Bloom Tex D", bloomW2_, bloomH2_, bloomTexD_, bloomRTV_D_, bloomSRV_D_)) {
        return false;
    }

    return true;
}

bool DiligentBackend::CreateUISceneTextures(SurfaceSize size) {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return true;
    }

    // 根据当前模式确定 RTV 格式
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM; // DirectComposition 使用非 sRGB
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }

    const uint32_t w   = size.Width;
    const uint32_t h   = size.Height;
    const uint32_t w6  = std::max(1u, size.Width / 6u);
    const uint32_t h6  = std::max(1u, size.Height / 6u);
    const uint32_t w12 = std::max(1u, size.Width / 12u);
    const uint32_t h12 = std::max(1u, size.Height / 12u);

    bool sceneSizeChanged = true;
    if (uiSceneColor_ != nullptr) {
        const auto& desc = uiSceneColor_->GetDesc();
        sceneSizeChanged = (desc.Width != w || desc.Height != h || desc.Format != rtvFormat);
    }

    const bool sizeChanged =
        sceneSizeChanged || (uiBlurW_ != w6 || uiBlurH_ != h6 || uiBlurW2_ != w12 || uiBlurH2_ != h12);
    if (!sizeChanged && uiSceneColor_ != nullptr && uiBlurTexA_ != nullptr && uiBlurTexB_ != nullptr &&
        uiBlurTexC_ != nullptr && uiBlurTexD_ != nullptr && uiAcrylicStrong_ != nullptr && uiAcrylicWeak_ != nullptr &&
        uiNoiseTex_ != nullptr) {
        return true;
    }

    uiBlurW_  = w6;
    uiBlurH_  = h6;
    uiBlurW2_ = w12;
    uiBlurH2_ = h12;

    auto createTex = [&](const char* name, TEXTURE_FORMAT fmt, uint32_t texW, uint32_t texH,
                         RefCntAutoPtr<ITexture>& outTex, RefCntAutoPtr<ITextureView>& outRTV,
                         RefCntAutoPtr<ITextureView>& outSRV) -> bool {
        TextureDesc texDesc{};
        texDesc.Name      = name;
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = texW;
        texDesc.Height    = texH;
        texDesc.MipLevels = 1;
        texDesc.Format    = fmt;
        texDesc.Usage     = USAGE_DEFAULT;
        texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;

        outTex.Release();
        outRTV.Release();
        outSRV.Release();

        device_->CreateTexture(texDesc, nullptr, &outTex);
        if (outTex == nullptr) {
            return false;
        }
        outRTV = outTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        outSRV = outTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        return outRTV != nullptr && outSRV != nullptr;
    };

    // 解析后的 LDR 场景纹理（与 SwapChain 颜色格式一致）
    if (!createTex("UI Scene Color", rtvFormat, w, h, uiSceneColor_, uiSceneRTV_, uiSceneSRV_)) {
        return false;
    }

    // UI Blur 纹理（低分辨率 float，用于更平滑的模糊采样）
    if (!createTex("UI Blur A (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiBlurTexA_, uiBlurRTV_A_,
                   uiBlurSRV_A_)) {
        return false;
    }
    if (!createTex("UI Blur B (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiBlurTexB_, uiBlurRTV_B_,
                   uiBlurSRV_B_)) {
        return false;
    }
    if (!createTex("UI Blur C (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiBlurTexC_, uiBlurRTV_C_,
                   uiBlurSRV_C_)) {
        return false;
    }
    if (!createTex("UI Blur D (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiBlurTexD_, uiBlurRTV_D_,
                   uiBlurSRV_D_)) {
        return false;
    }

    // Acrylic 合成输出（同分辨率）
    if (!createTex("UI Acrylic Strong (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiAcrylicStrong_,
                   uiAcrylicRTV_Strong_, uiAcrylicSRV_Strong_)) {
        return false;
    }
    if (!createTex("UI Acrylic Weak (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiAcrylicWeak_,
                   uiAcrylicRTV_Weak_, uiAcrylicSRV_Weak_)) {
        return false;
    }

    // 噪点纹理（全分辨率、一次性上传；避免依赖 wrap sampler）
    {
        uiNoiseSRV_.Release();
        uiNoiseTex_.Release();

        std::vector<uint8_t> noise;
        noise.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        std::mt19937                       gen{1337u};
        std::uniform_int_distribution<int> rnd(0, 255);
        for (size_t i = 0; i < noise.size(); i += 4u) {
            const uint8_t v = static_cast<uint8_t>(rnd(gen));
            noise[i + 0u]   = v;
            noise[i + 1u]   = v;
            noise[i + 2u]   = v;
            noise[i + 3u]   = 255u;
        }

        TextureDesc texDesc{};
        texDesc.Name      = "UI Noise Texture";
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = w;
        texDesc.Height    = h;
        texDesc.MipLevels = 1;
        texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
        texDesc.BindFlags = BIND_SHADER_RESOURCE;
        texDesc.Usage     = USAGE_IMMUTABLE;

        TextureSubResData subRes{};
        subRes.pData  = noise.data();
        subRes.Stride = static_cast<Uint32>(w * 4u);

        TextureData texData{};
        texData.NumSubresources = 1;
        texData.pSubResources   = &subRes;

        device_->CreateTexture(texDesc, &texData, &uiNoiseTex_);
        if (uiNoiseTex_ == nullptr) {
            return false;
        }
        uiNoiseSRV_ = uiNoiseTex_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        if (uiNoiseSRV_ == nullptr) {
            return false;
        }
    }

    return true;
}

Diligent::ITextureView* DiligentBackend::GetOrCreateLogControlIconSRV(
    bool pausedState /* true=resume icon, false=pause icon */) {
    if (device_ == nullptr) {
        return nullptr;
    }

    auto& tex = pausedState ? logResumeIconTex_ : logPauseIconTex_;
    auto& srv = pausedState ? logResumeIconSRV_ : logPauseIconSRV_;
    if (srv != nullptr) {
        return srv.RawPtr();
    }

    const int      px  = GeneratedIcons::kLogIconPx;
    const uint32_t rgb = GeneratedIcons::kLogIconRgb;
    const uint8_t* a   = pausedState ? GeneratedIcons::kLogResumeAlpha : GeneratedIcons::kLogPauseAlpha;
    if (px <= 0 || a == nullptr) {
        return nullptr;
    }

    const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>(rgb & 0xFFu);

    std::vector<uint8_t> rgba(static_cast<size_t>(px) * static_cast<size_t>(px) * 4u, 0u);
    for (int i = 0; i < px * px; i++) {
        rgba[static_cast<size_t>(i) * 4u + 0u] = r;
        rgba[static_cast<size_t>(i) * 4u + 1u] = g;
        rgba[static_cast<size_t>(i) * 4u + 2u] = b;
        rgba[static_cast<size_t>(i) * 4u + 3u] = a[i];
    }

    TextureDesc texDesc{};
    texDesc.Name      = pausedState ? "Log Resume Icon" : "Log Pause Icon";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = static_cast<Uint32>(px);
    texDesc.Height    = static_cast<Uint32>(px);
    texDesc.MipLevels = 1;
    texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_IMMUTABLE;

    TextureSubResData subRes{};
    subRes.pData  = rgba.data();
    subRes.Stride = static_cast<Uint32>(px * 4u);

    TextureData texData{};
    texData.NumSubresources = 1;
    texData.pSubResources   = &subRes;

    device_->CreateTexture(texDesc, &texData, &tex);
    if (tex == nullptr) {
        return nullptr;
    }

    srv = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    return srv.RawPtr();
}

void DiligentBackend::RenderUISceneForUI() {
    if (immediateContext_ == nullptr || !IsInitialized() || fullscreenQuadPSO_ == nullptr ||
        fullscreenQuadSRB_ == nullptr || offscreenSRV_ == nullptr || bloomSRV_B_ == nullptr || uiSceneRTV_ == nullptr) {
        return;
    }

    ITextureView* rtv = uiSceneRTV_.RawPtr();
    immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // 热路径：避免每帧重复字符串查找
    auto* texVar   = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    auto* bloomVar = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_BloomTexture");
    if (texVar != nullptr) {
        texVar->Set(offscreenSRV_);
    }
    if (bloomVar != nullptr) {
        bloomVar->Set(bloomSRV_B_);
    }

    immediateContext_->SetPipelineState(fullscreenQuadPSO_);
    immediateContext_->CommitShaderResources(fullscreenQuadSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                        SET_VERTEX_BUFFERS_FLAG_RESET);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = kDrawVerifyFlags;
    immediateContext_->Draw(draw);
}

void DiligentBackend::RenderUIBlur() {
    if (immediateContext_ == nullptr) {
        return;
    }
    if (uiSceneSRV_ == nullptr || uiBlurRTV_A_ == nullptr || uiBlurSRV_A_ == nullptr || uiBlurRTV_B_ == nullptr ||
        uiBlurSRV_B_ == nullptr || uiBlurRTV_C_ == nullptr || uiBlurSRV_C_ == nullptr || uiBlurRTV_D_ == nullptr ||
        uiBlurSRV_D_ == nullptr) {
        return;
    }
    if (bloomDownsamplePSO_ == nullptr || bloomDownsampleSRB_ == nullptr || bloomBlurPSO_ == nullptr ||
        bloomBlurSRB_ == nullptr || bloomBlurConstants_ == nullptr) {
        return;
    }
    if (uiBlurW_ == 0 || uiBlurH_ == 0) {
        return;
    }

    auto updateBlurCB = [&](float texelX, float texelY, float offset, float threshold) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomBlurConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BlurCB {
                float texelSize[2];
                float offset;
                float threshold;
            };
            auto* cb         = static_cast<BlurCB*>(mapped);
            cb->texelSize[0] = texelX;
            cb->texelSize[1] = texelY;
            cb->offset       = offset;
            cb->threshold    = threshold;
            immediateContext_->UnmapBuffer(bloomBlurConstants_, MAP_WRITE);
        }
    };

    // 热路径：避免在 blur ping-pong 循环里反复做字符串查找
    auto* downTexVar = bloomDownsampleSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    auto* blurTexVar = bloomBlurSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");

    // --- 1) downsample: uiScene(full) -> uiBlurA(1/6), 不做 bright-pass ---
    {
        ITextureView* rtv = uiBlurRTV_A_.RawPtr();
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = static_cast<float>(uiBlurW_);
        vp.Height   = static_cast<float>(uiBlurH_);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp, 0, 0);

        const float texelX = (surfaceSize_.Width > 0) ? (1.0f / static_cast<float>(surfaceSize_.Width)) : 0.0f;
        const float texelY = (surfaceSize_.Height > 0) ? (1.0f / static_cast<float>(surfaceSize_.Height)) : 0.0f;
        updateBlurCB(texelX, texelY, 0.0f, 0.0f);

        if (downTexVar != nullptr) {
            downTexVar->Set(uiSceneSRV_);
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // --- 2) Kawase blur ping-pong: uiBlurA <-> uiBlurB ---
    // blurStrength 滑条是 float：旧实现用 int 决定 iterations，导致“滑条无级、效果有级”。
    // 这里改为固定迭代次数 + 连续缩放 offset，让 blurStrength 真正连续生效。
    const float            blurStrength   = (appState_ != nullptr) ? appState_->ui.blurStrength : 2.0f;
    static constexpr float offsets[]      = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    static constexpr int   kMaxIterations = static_cast<int>(sizeof(offsets) / sizeof(offsets[0])); // 8（偶数）
    const float            strength       = std::clamp(blurStrength, 0.0f, 5.0f);
    const float            scale          = strength / 5.0f; // 0..1
    auto                   scaledOffset   = [&](float base) -> float {
        // Shader: off = g_TexelSize * (g_Offset + 0.5)
        // 让 scale=0 时 off=0，scale=1 时保持旧行为：
        // g_Offset = scale*(base+0.5) - 0.5
        return scale * (base + 0.5f) - 0.5f;
    };
    const int iterations = kMaxIterations;

    const float texelX6 = 1.0f / static_cast<float>(uiBlurW_);
    const float texelY6 = 1.0f / static_cast<float>(uiBlurH_);

    for (int i = 1; i < iterations; ++i) {
        const bool    writeToB = (i % 2 == 1);
        ITextureView* outRTV   = writeToB ? uiBlurRTV_B_.RawPtr() : uiBlurRTV_A_.RawPtr();
        ITextureView* inSRV    = writeToB ? uiBlurSRV_A_.RawPtr() : uiBlurSRV_B_.RawPtr();

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(texelX6, texelY6, scaledOffset(offsets[i]), 0.0f);

        if (blurTexVar != nullptr) {
            blurTexVar->Set(inSRV);
        }

        immediateContext_->SetPipelineState(bloomBlurPSO_);
        immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // --- 3) secondary (1/12): downsample uiBlurB(1/6) -> uiBlurC(1/12), 再做 2 次小 offset 模糊 ---
    if (uiBlurW2_ == 0 || uiBlurH2_ == 0) {
        return;
    }
    {
        ITextureView* rtv = uiBlurRTV_C_.RawPtr();
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp2{};
        vp2.TopLeftX = 0.0f;
        vp2.TopLeftY = 0.0f;
        vp2.Width    = static_cast<float>(uiBlurW2_);
        vp2.Height   = static_cast<float>(uiBlurH2_);
        vp2.MinDepth = 0.0f;
        vp2.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp2, 0, 0);

        updateBlurCB(texelX6, texelY6, 0.0f, 0.0f);
        if (downTexVar != nullptr) {
            downTexVar->Set(uiBlurSRV_B_.RawPtr());
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    const float            texelX12           = 1.0f / static_cast<float>(uiBlurW2_);
    const float            texelY12           = 1.0f / static_cast<float>(uiBlurH2_);
    static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
    static constexpr int   secondaryIterations =
        static_cast<int>(sizeof(secondaryOffsets) / sizeof(secondaryOffsets[0])); // 偶数，最终结果落回 C

    for (int i = 0; i < secondaryIterations; ++i) {
        const bool    writeToD = (i % 2 == 0); // C->D->C...
        ITextureView* outRTV   = writeToD ? uiBlurRTV_D_.RawPtr() : uiBlurRTV_C_.RawPtr();
        ITextureView* inSRV    = writeToD ? uiBlurSRV_C_.RawPtr() : uiBlurSRV_D_.RawPtr();

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(texelX12, texelY12, scaledOffset(secondaryOffsets[i]), 0.0f);
        if (blurTexVar != nullptr) {
            blurTexVar->Set(inSRV);
        }
        immediateContext_->SetPipelineState(bloomBlurPSO_);
        immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }
}

void DiligentBackend::RenderAcrylicComposite() {
    if (immediateContext_ == nullptr) {
        return;
    }
    if (appState_ != nullptr && !appState_->ui.enableBlur) {
        return;
    }
    if (acrylicPSO_ == nullptr || acrylicSRB_ == nullptr || acrylicConstants_ == nullptr) {
        return;
    }
    if (uiAcrylicRTV_Strong_ == nullptr || uiAcrylicSRV_Strong_ == nullptr || uiAcrylicRTV_Weak_ == nullptr ||
        uiAcrylicSRV_Weak_ == nullptr) {
        return;
    }
    if (uiBlurSRV_B_ == nullptr || uiBlurSRV_C_ == nullptr) {
        return;
    }
    if (uiBlurW_ == 0 || uiBlurH_ == 0 || uiBlurW2_ == 0 || uiBlurH2_ == 0) {
        return;
    }

    const bool isDark = (appState_ != nullptr) ? appState_->ui.isDarkMode : true;

    auto updateCB = [&](float tintR, float tintG, float tintB, float baseOpacity, float saturation, float adaptive,
                        float exclusionStrength) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(acrylicConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct AcrylicCB {
                float Tint[4];
                float Params[4];
            };
            auto* cb    = static_cast<AcrylicCB*>(mapped);
            cb->Tint[0] = tintR;
            cb->Tint[1] = tintG;
            cb->Tint[2] = tintB;
            cb->Tint[3] = baseOpacity;

            cb->Params[0] = saturation;
            cb->Params[1] = adaptive;
            cb->Params[2] = isDark ? 1.0f : 0.0f;
            cb->Params[3] = exclusionStrength;

            immediateContext_->UnmapBuffer(acrylicConstants_, MAP_WRITE);
        }
    };

    // 热路径：避免每次合成都做字符串查找
    auto* acrylicTexVar = acrylicSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");

    auto drawComposite = [&](ITextureView* outRTV, uint32_t w, uint32_t h, ITextureView* inSRV) {
        if (outRTV == nullptr || inSRV == nullptr) {
            return;
        }

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = static_cast<float>(w);
        vp.Height   = static_cast<float>(h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp, 0, 0);

        if (acrylicTexVar != nullptr) {
            acrylicTexVar->Set(inSRV);
        }

        immediateContext_->SetPipelineState(acrylicPSO_);
        immediateContext_->CommitShaderResources(acrylicSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    };

    // Strong Acrylic (1/6)：用于窗口背景
    {
        // 经验值：Acrylic 通常比原背景更“鲜艳”，并用较高 opacity 稳定可读性
        const float saturation = 1.35f;
        const float adaptive   = 0.35f;
        const float excl       = 1.0f;

        if (isDark) {
            updateCB(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 180.0f / 255.0f, saturation, adaptive, excl);
        } else {
            updateCB(245.0f / 255.0f, 245.0f / 255.0f, 255.0f / 255.0f, 150.0f / 255.0f, saturation, adaptive, excl);
        }
        drawComposite(uiAcrylicRTV_Strong_.RawPtr(), uiBlurW_, uiBlurH_, uiBlurSRV_B_.RawPtr());
    }

    // Weak Acrylic (1/12)：用于折叠区域/次级背景
    {
        const float saturation = 1.30f;
        const float adaptive   = 0.30f;
        const float excl       = 1.0f;

        if (isDark) {
            updateCB(35.0f / 255.0f, 35.0f / 255.0f, 40.0f / 255.0f, 160.0f / 255.0f, saturation, adaptive, excl);
        } else {
            updateCB(250.0f / 255.0f, 250.0f / 255.0f, 255.0f / 255.0f, 140.0f / 255.0f, saturation, adaptive, excl);
        }
        drawComposite(uiAcrylicRTV_Weak_.RawPtr(), uiBlurW2_, uiBlurH2_, uiBlurSRV_C_.RawPtr());
    }
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
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_) ||
        starConstants_ == nullptr) {
        return false;
    }

    const auto sources = GetStarShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "Starfield VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language, renderStateCache_);
    const auto ps =
        CreateShaderFromSource(device_, "Starfield PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language, renderStateCache_);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "Starfield PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets  = 1;
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
    CreateGraphicsPSO(device_, psoCI, &starPSO_, renderStateCache_);
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

    // Vulkan 修复：显式将所有粒子缓冲区转换到正确的初始资源状态。
    // 在 Vulkan 下，新创建的缓冲区处于 RESOURCE_STATE_UNKNOWN 状态，
    // 必须在首次使用前转换到正确状态，否则会导致验证层错误或崩溃。
    // - renderIdx 缓冲区将用于渲染（SRV 读取）
    // - readIdx 缓冲区将用于计算输入（SRV 读取）
    // - writeIdx 缓冲区将用于计算输出（UAV 写入）
    {
        StateTransitionDesc barriers[kParticleBufferCount] = {};
        for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
            barriers[i].pResource      = particleBuffers_[i];
            barriers[i].OldState       = RESOURCE_STATE_UNKNOWN;
            barriers[i].NewState       = RESOURCE_STATE_SHADER_RESOURCE;
            barriers[i].TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
            barriers[i].Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        }
        immediateContext_->TransitionResourceStates(kParticleBufferCount, barriers);
    }

    return true;
}

bool DiligentBackend::CreateParticlePSO() {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_) ||
        particleConstants_ == nullptr) {
        return false;
    }

    const auto sources = GetSaturnParticleShaderSources(backend_);
    if (sources.Vertex == nullptr || sources.Fragment == nullptr) {
        return false;
    }

    const auto vs =
        CreateShaderFromSource(device_, "SaturnParticle VS", SHADER_TYPE_VERTEX, sources.Vertex, sources.Language, renderStateCache_);
    const auto ps =
        CreateShaderFromSource(device_, "SaturnParticle PS", SHADER_TYPE_PIXEL, sources.Fragment, sources.Language, renderStateCache_);
    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnParticle PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets             = 1;
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
    CreateGraphicsPSO(device_, psoCI, &particlePSO_, renderStateCache_);
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
        DebugLog::Instance().Add(LogLevel::Error,
                                 "[CreateParticleComputePSO] GetSaturnComputeShaderSource() returned nullptr");
        return false;
    }

    const auto cs =
        CreateShaderFromSource(device_, "SaturnCompute CS", SHADER_TYPE_COMPUTE, csSrc.Source, csSrc.Language, renderStateCache_);
    if (cs == nullptr) {
        DebugLog::Instance().Add(LogLevel::Error, "[CreateParticleComputePSO] Compute shader compilation failed");
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
    CreateComputePSO(device_, psoCI, &particleComputePSO_, renderStateCache_);
    if (particleComputePSO_ == nullptr) {
        DebugLog::Instance().Add(LogLevel::Error, "[CreateParticleComputePSO] CreateComputePipelineState failed");
        return false;
    }

    if (auto* var = particleComputePSO_->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "ComputeConstants");
        var != nullptr) {
        var->Set(particleComputeConstants_);
    } else {
        DebugLog::Instance().Add(
            LogLevel::Error, "[CreateParticleComputePSO] GetStaticVariableByName(ComputeConstants) returned nullptr");
        return false;
    }

    particleComputePSO_->CreateShaderResourceBinding(&particleComputeSRB_, true);
    if (particleComputeSRB_ == nullptr) {
        DebugLog::Instance().Add(LogLevel::Error, "[CreateParticleComputePSO] CreateShaderResourceBinding failed");
    }
    return particleComputeSRB_ != nullptr;
}

bool DiligentBackend::CreateOffscreenRenderTarget(SurfaceSize size) {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return false;
    }

    TextureDesc texDesc{};
    texDesc.Name      = "Offscreen Color";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = size.Width;
    texDesc.Height    = size.Height;
    texDesc.MipLevels = 1;
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
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = rtvFormat;
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

    CreateGraphicsPSO(device_, psoCI, &sevenSegPSO_, renderStateCache_);
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
        DebugLog::Instance().AddOnce("SimulateParticles_Null", LogLevel::Warn,
                                     "[SimulateParticles] skipped: compute pipeline not ready");
        return;
    }
    if (particleCount_ == 0) {
        return;
    }
    // 复刻 OpenGL 的三缓冲索引用法（见 OpenGL: DoubleBufferSSBO::Swap）：
    // - readIdx：本帧计算着色器的输入
    // - writeIdx：本帧计算着色器的输出
    // - renderIdx：本帧渲染使用的数据（Swap 后等于上一帧的 readIdx）
    if (particleSRVs_[particleReadIdx_] == nullptr || particleUAVs_[particleWriteIdx_] == nullptr) {
        DebugLog::Instance().AddOnce("SimulateParticles_SrvUavNull", LogLevel::Warn,
                                     "[SimulateParticles] skipped: SRV/UAV is null");
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
    // 注意：使用 RESOURCE_STATE_UNKNOWN 让 Diligent 自动检测当前状态，避免第一帧时状态不匹配
    {
        StateTransitionDesc barrier{};
        barrier.pResource      = particleBuffers_[particleWriteIdx_];
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
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
    if (!IsInitialized() || immediateContext_ == nullptr) {
        return;
    }

    ITextureView* pRTV = GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }

    // 深度缓冲：DirectComposition 模式下暂不使用深度缓冲（后续可扩展）
    ITextureView* pDSV = swapChain_ ? swapChain_->GetDepthBufferDSV() : nullptr;

    immediateContext_->SetRenderTargets(1, &pRTV, pDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // 透明窗口模式下需要清为 alpha=0（否则 DWM 会把 client 区域当作不透明）。
    float clearColor[4] = {0.05f, 0.07f, 0.10f, 1.0f};
    if (appState_ != nullptr && appState_->backdrop.useTransparent) {
        clearColor[0] = 0.0f;
        clearColor[1] = 0.0f;
        clearColor[2] = 0.0f;
        clearColor[3] = 0.0f;
    }
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

    // 添加空指针检查，避免 Vulkan 上因无效 RTV 导致崩溃
    if (offscreenRTV_ == nullptr) {
        DebugLog::Instance().AddOnce("RenderOffscreen_NoRTV", LogLevel::Warn,
                                     "[RenderOffscreen] offscreenRTV_ is null, skipping");
        return;
    }

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
        starsDraw.NumVertices = 6;
        uint32_t starLodCount = starCount_;
        if (appState_ != nullptr && appState_->render.pixelRatio < 0.85f) {
            // OpenGL 版：低 pixelRatio 时绘制 60% 星星
            starLodCount = static_cast<uint32_t>(static_cast<float>(kStarCountBase) * kStarLodRatio);
            if (starLodCount > starCount_) {
                starLodCount = starCount_;
            }
        }
        starsDraw.NumInstances = starLodCount;
        starsDraw.Flags        = kDrawVerifyFlags;
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

        // Hand tracking (对齐 OpenGL)：
        // - 无手：沿用自动动画（正弦）
        // - 有手：由 HandTracker 的 scale/rot 映射到土星旋转/缩放，并做 dt 相关平滑
        HandTracking::Sample handSample{};
        bool                 hasHand = false;
        if (handTracker_ != nullptr && handTracker_->GetStatus() == HandTracking::Status::Ready) {
            handSample = handTracker_->GetLatestSample();
            hasHand    = handSample.hasHand;
        }

        float targetScale = 1.0f;
        float targetRotX  = 0.4f;
        float targetRotY  = 0.0f;

        float perFrameAlpha = 0.08f; // OpenGL auto animation smoothing

        if (!hasHand) {
            animAutoTime_ += dt * (0.005f * 180.0f);
            targetScale = 1.0f + std::sin(animAutoTime_) * 0.2f;
            targetRotX  = 0.4f + std::sin(animAutoTime_ * 0.3f) * 0.15f;
            targetRotY  = 0.0f;
        } else {
            targetScale = handSample.scale;

            float sensitivity = 1.0f;
            bool  invertX     = false;
            bool  invertY     = false;
            if (appState_ != nullptr) {
                sensitivity = std::clamp(appState_->handParams.sensitivity, 0.1f, 3.0f);
                invertX     = appState_->handParams.invertX;
                invertY     = appState_->handParams.invertY;
            }

            auto        applyInvert = [](float v, bool inv) -> float { return inv ? (1.0f - v) : v; };
            const float rotX01      = applyInvert(handSample.rotX, invertX);
            const float rotY01      = applyInvert(handSample.rotY, invertY);

            // 对齐 OpenGL 映射：
            // targetRotX = -0.6 + rotY*1.6
            // targetRotY = (rotX-0.5)*2
            targetRotX = (-0.6f + rotY01 * 1.6f) * sensitivity;
            targetRotY = ((rotX01 - 0.5f) * 2.0f) * sensitivity;

            perFrameAlpha = 0.25f; // OpenGL hand-driven smoothing
        }

        const float alpha = 1.0f - std::pow(1.0f - perFrameAlpha, dt * 180.0f);
        animScale_        = animScale_ + (targetScale - animScale_) * alpha;
        animRotX_         = animRotX_ + (targetRotX - animRotX_) * alpha;
        animRotY_         = animRotY_ + (targetRotY - animRotY_) * alpha;

        // 阶段 3（第 2 步）：接入 GPU ComputeSaturn（物理模拟）并用三缓冲轮转避免读写冲突。
        // uHandHas：1 表示有手，0 表示无手（只影响 compute 的交互分支）。
        SimulateParticles(dt, animScale_, hasHand ? 1.0f : 0.0f);

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

                // 对齐 OpenGL：pixelRatio / densityComp 由动态 LOD 或 UI 控制。
                const float pixelRatio =
                    (appState_ != nullptr && appState_->render.pixelRatio > 0.0f) ? appState_->render.pixelRatio : 1.0f;
                const float densityComp = (appState_ != nullptr) ? appState_->render.densityComp
                                                                 : ComputeDensityComp(particleCount_, pixelRatio);

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
            ia.Flags                            = kDrawVerifyFlags;
            ia.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            immediateContext_->DrawIndirect(ia);
        }
    }
}

void DiligentBackend::RenderBloom() {
    if (immediateContext_ == nullptr || device_ == nullptr) {
        return;
    }
    if (offscreenSRV_ == nullptr || bloomRTV_A_ == nullptr || bloomSRV_A_ == nullptr || bloomRTV_B_ == nullptr ||
        bloomSRV_B_ == nullptr) {
        return;
    }
    if (bloomDownsamplePSO_ == nullptr || bloomDownsampleSRB_ == nullptr || bloomBlurPSO_ == nullptr ||
        bloomBlurSRB_ == nullptr || bloomBlurConstants_ == nullptr) {
        return;
    }
    if (bloomW_ == 0 || bloomH_ == 0) {
        return;
    }

    // 视口设为 Bloom 分辨率
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(bloomW_);
    vp.Height   = static_cast<float>(bloomH_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, bloomW_, bloomH_);

    auto updateBlurCB = [&](float texelX, float texelY, float offset, float threshold) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomBlurConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BlurCB {
                float texelSize[2];
                float offset;
                float threshold;
            };
            auto* cb         = static_cast<BlurCB*>(mapped);
            cb->texelSize[0] = texelX;
            cb->texelSize[1] = texelY;
            cb->offset       = offset;
            cb->threshold    = threshold;
            immediateContext_->UnmapBuffer(bloomBlurConstants_, MAP_WRITE);
        }
    };

    // 热路径：RenderBloom 每帧多次 ping-pong，避免循环内反复 GetVariableByName()
    auto* downTexVar = bloomDownsampleSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    auto* blurTexVar = bloomBlurSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");

    // Pass 0: bright-pass downsample（offscreen -> bloomA）
    {
        ITextureView* rtv = bloomRTV_A_;
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // offscreen texel size
        uint32_t srcW = surfaceSize_.Width;
        uint32_t srcH = surfaceSize_.Height;
        if (offscreenColor_ != nullptr) {
            const auto& desc = offscreenColor_->GetDesc();
            srcW             = desc.Width;
            srcH             = desc.Height;
        }
        const float texelX = (srcW > 0) ? (1.0f / static_cast<float>(srcW)) : 0.0f;
        const float texelY = (srcH > 0) ? (1.0f / static_cast<float>(srcH)) : 0.0f;
        updateBlurCB(texelX, texelY, 0.0f, 1.0f);

        if (downTexVar != nullptr) {
            downTexVar->Set(offscreenSRV_);
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // 无 VB，使用 SV_VertexID/gl_VertexIndex
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);
        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // Pass 1..N: Kawase blur ping-pong（bloomA <-> bloomB）
    // blurStrength 滑条是 float：旧实现用 int 决定 iterations，导致“滑条无级、效果有级”。
    // 这里改为固定迭代次数 + 连续缩放 offset，让 blurStrength 真正连续生效。
    const float            blurStrength   = (appState_ != nullptr) ? appState_->ui.blurStrength : 2.0f;
    static constexpr float offsets[]      = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    static constexpr int   kMaxIterations = static_cast<int>(sizeof(offsets) / sizeof(offsets[0])); // 8（偶数）
    const float            strength       = std::clamp(blurStrength, 0.0f, 5.0f);
    const float            scale          = strength / 5.0f; // 0..1
    auto                   scaledOffset   = [&](float base) -> float {
        // Shader: off = g_TexelSize * (g_Offset + 0.5)
        // 让 scale=0 时 off=0，scale=1 时保持旧行为：
        // g_Offset = scale*(base+0.5) - 0.5
        return scale * (base + 0.5f) - 0.5f;
    };
    const int iterations = kMaxIterations;

    const float bloomTexelX = 1.0f / static_cast<float>(bloomW_);
    const float bloomTexelY = 1.0f / static_cast<float>(bloomH_);

    for (int i = 1; i < iterations; ++i) {
        const bool    writeToB = (i % 2 == 1);
        ITextureView* outRTV   = writeToB ? bloomRTV_B_ : bloomRTV_A_;
        ITextureView* inSRV    = writeToB ? bloomSRV_A_ : bloomSRV_B_;

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(bloomTexelX, bloomTexelY, scaledOffset(offsets[i]), 0.0f);

        if (blurTexVar != nullptr) {
            blurTexVar->Set(inSRV);
        }

        immediateContext_->SetPipelineState(bloomBlurPSO_);
        immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // ========== 次级模糊 (1/12 分辨率，用于折叠区域 Acrylic 效果) ==========
    // 从 bloomSRV_B_ (1/6) 降采样到 bloomTexC_ (1/12)，再做 2 次模糊。
    // 注意：不能 in-place（同一纹理同时 RTV+SRV）——在 D3D12/Vulkan 下属于未定义/不允许行为。
    if (bloomTexC_ != nullptr && bloomRTV_C_ != nullptr && bloomSRV_C_ != nullptr && bloomTexD_ != nullptr &&
        bloomRTV_D_ != nullptr && bloomSRV_D_ != nullptr) {
        const float texelX2 = 1.0f / static_cast<float>(bloomW2_);
        const float texelY2 = 1.0f / static_cast<float>(bloomH2_);

        // 第一步：从 1/6 降采样到 1/12（使用 downsample shader）
        {
            ITextureView* rtv = bloomRTV_C_.RawPtr();
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            Viewport vp2{};
            vp2.TopLeftX = 0.0f;
            vp2.TopLeftY = 0.0f;
            vp2.Width    = static_cast<float>(bloomW2_);
            vp2.Height   = static_cast<float>(bloomH2_);
            vp2.MinDepth = 0.0f;
            vp2.MaxDepth = 1.0f;
            immediateContext_->SetViewports(1, &vp2, 0, 0);

            // 使用 1/6 纹理作为输入
            if (downTexVar != nullptr) {
                downTexVar->Set(bloomSRV_B_.RawPtr());
            }

            immediateContext_->SetPipelineState(bloomDownsamplePSO_);
            immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                SET_VERTEX_BUFFERS_FLAG_RESET);

            DrawAttribs draw{};
            draw.NumVertices = 4;
            draw.Flags       = kDrawVerifyFlags;
            immediateContext_->Draw(draw);
        }

        // 第二步：2 次模糊迭代（使用较小的 offset）
        // 由于分辨率更低，使用较小的 offset 值。iterations 设为偶数，确保最终结果落在 bloomTexC_ 中。
        static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
        static constexpr int   secondaryIterations =
            static_cast<int>(sizeof(secondaryOffsets) / sizeof(secondaryOffsets[0]));

        for (int i = 0; i < secondaryIterations; ++i) {
            const bool    writeToD = (i % 2 == 0); // C->D->C...
            ITextureView* outRTV   = writeToD ? bloomRTV_D_.RawPtr() : bloomRTV_C_.RawPtr();
            ITextureView* inSRV    = writeToD ? bloomSRV_C_.RawPtr() : bloomSRV_D_.RawPtr();

            immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            updateBlurCB(texelX2, texelY2, scaledOffset(secondaryOffsets[i]), 0.0f);

            if (blurTexVar != nullptr) {
                blurTexVar->Set(inSRV);
            }

            immediateContext_->SetPipelineState(bloomBlurPSO_);
            immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                SET_VERTEX_BUFFERS_FLAG_RESET);

            DrawAttribs draw{};
            draw.NumVertices = 4;
            draw.Flags       = kDrawVerifyFlags;
            immediateContext_->Draw(draw);
        }
    }
}

void DiligentBackend::BlitOffscreenToBackBuffer() {
    if (!IsInitialized() || immediateContext_ == nullptr || fullscreenQuadPSO_ == nullptr ||
        fullscreenQuadSRB_ == nullptr || offscreenSRV_ == nullptr) {
        return;
    }

    ITextureView* pBackBufferRTV = GetCurrentBackBufferRTV();
    if (pBackBufferRTV == nullptr) {
        return;
    }

    // 全屏合成：offscreen + bloom + tone mapping
    immediateContext_->SetRenderTargets(1, &pBackBufferRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // Bloom 强度（由 UI 控制）
    if (bloomConstants_ != nullptr) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BloomCB {
                float strength;
                float transparent;
                float isD3D11; // pad[0] -> isD3D11
                float pad;
            };

            auto* cb        = static_cast<BloomCB*>(mapped);
            cb->strength    = bloomEnabled_ ? std::max(0.0f, bloomStrength_) : 0.0f;
            cb->transparent = (appState_ != nullptr && appState_->backdrop.useTransparent) ? 1.0f : 0.0f;
            cb->isD3D11     = (backend_ == Backend::D3D11) ? 1.0f : 0.0f;
            cb->pad         = 0.0f;
            immediateContext_->UnmapBuffer(bloomConstants_, MAP_WRITE);
        }
    }

    // 绑定原始 HDR 纹理 + Bloom 纹理（热路径：避免每帧重复字符串查找）
    auto* texVar   = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    auto* bloomVar = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_BloomTexture");
    if (texVar != nullptr) {
        texVar->Set(offscreenSRV_);
    }
    // Bloom：使用 RenderBloom() 的最终结果（约 1/6 分辨率，采样时自动线性滤波）
    if (bloomVar != nullptr) {
        if (bloomSRV_B_ != nullptr) {
            bloomVar->Set(bloomSRV_B_);
        } else {
            bloomVar->Set(offscreenSRV_);
        }
    }

    immediateContext_->SetPipelineState(fullscreenQuadPSO_);
    immediateContext_->CommitShaderResources(fullscreenQuadSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = kDrawVerifyFlags;
    immediateContext_->Draw(draw);
}

void DiligentBackend::RenderFrame() {
    if (!IsInitialized()) {
        return;
    }

    // 延迟 resize：避免在 WndProc 的 WM_SIZE 里做重资源操作导致卡顿/假死。
    // 如果 ResizeBuffers 因 DXGI_ERROR_INVALID_CALL 暂时失败，保持 pending 状态，下一帧继续尝试。
    if (hasPendingResize_) {
        const auto target = pendingResize_;
        Resize(target);
        if (surfaceSize_.Width == target.Width && surfaceSize_.Height == target.Height) {
            hasPendingResize_ = false;
        }
    }

    // HandTracker：每帧轮询一次初始化状态（非阻塞）。
    if (handTracker_) {
        handTracker_->Tick();
    }

    // 计算帧时间和 FPS（移动平均）
    const auto now     = std::chrono::steady_clock::now();
    float      frameDt = 0.0f;
    if (lastFrameTime_ != std::chrono::steady_clock::time_point{}) {
        frameDt = std::chrono::duration<float>(now - lastFrameTime_).count();
        if (frameDt > 0.0f && frameDt < 1.0f) {
            frameDtSamples_[fpsSampleIndex_] = frameDt;
            fpsSampleIndex_                  = (fpsSampleIndex_ + 1) % kFpsSampleCount;

            // 计算平均 FPS（帧时间调和平均：N / Σdt）
            float sumDt = 0.0f;
            for (int i = 0; i < kFpsSampleCount; ++i) {
                sumDt += frameDtSamples_[i];
            }
            currentFps_ = (sumDt > 0.0f) ? (static_cast<float>(kFpsSampleCount) / sumDt) : 60.0f;

            // FPS 历史曲线采样（低频）
            fpsHistorySampleTimer_ += frameDt;
            if (fpsHistorySampleTimer_ >= kFpsHistorySampleInterval) {
                // 使用减法保留超出时间（与 OpenGL 版一致）
                fpsHistorySampleTimer_ -= kFpsHistorySampleInterval;
                fpsHistory_[fpsHistoryIndex_] = currentFps_;
                fpsHistoryIndex_              = (fpsHistoryIndex_ + 1) % kFpsHistorySize;
                // 滚动动画时间与采样计时器同步
                fpsGraphScrollAnimTime_ = fpsHistorySampleTimer_;
            } else {
                // 正常累加动画时间
                fpsGraphScrollAnimTime_ += frameDt;
            }
        }
    }
    lastFrameTime_ = now;

    // 动态 LOD（对齐 OpenGL）：每 0.5s 根据平滑 FPS 自动调节粒子数 / pixelRatio，并更新密度补偿。
    if (appState_ != nullptr && frameDt > 0.0f) {
        const uint32_t prevBasisCount =
            lastLodBasisValid_ ? lastLodParticleCount_ : appState_->render.activeParticleCount;
        const float prevBasisPR = lastLodBasisValid_ ? lastLodPixelRatio_ : appState_->render.pixelRatio;

        // 确保初始值合理（Diligent 入口不一定会调用 AppState::InitDefaults）
        if (appState_->render.activeParticleCount == 0) {
            appState_->render.activeParticleCount = (particleCount_ != 0) ? particleCount_ : kParticleCountMax;
        }
        if (appState_->render.pixelRatio <= 0.0f) {
            appState_->render.pixelRatio = 1.0f;
        }

        lodUpdateTimer_ += frameDt;
        if (lodUpdateTimer_ >= 0.5f) {
            lodUpdateTimer_ = 0.0f;

            if (!appState_->lod.locked) {
                const float smoothedFps = currentFps_;

                bool particleCountChanged = false;
                bool pixelRatioChanged    = false;

                // OpenGL 版阈值与步进：
                // - 低于 38 FPS：优先降低粒子数（*0.95），降到 MIN 后再降 pixelRatio（-0.03，最低 0.7）
                // - 高于 57 FPS：优先提高 pixelRatio（+0.03，最高 1.0），再提高粒子数（*1.05，最高 MAX）
                if (smoothedFps < 38.0f) {
                    if (appState_->render.activeParticleCount > kParticleCountMin) {
                        uint32_t newCount =
                            static_cast<uint32_t>(static_cast<float>(appState_->render.activeParticleCount) * 0.95f);
                        newCount = std::max(newCount, kParticleCountMin);
                        if (newCount != appState_->render.activeParticleCount) {
                            appState_->render.activeParticleCount = newCount;
                            particleCountChanged                  = true;
                            appState_->lod.lastDecision           = 1;
                        }
                    } else if (appState_->render.pixelRatio > 0.7f) {
                        float pr = appState_->render.pixelRatio - 0.03f;
                        pr       = std::max(pr, 0.7f);
                        if (std::abs(pr - appState_->render.pixelRatio) > 1e-6f) {
                            appState_->render.pixelRatio = pr;
                            pixelRatioChanged            = true;
                            appState_->lod.lastDecision  = 2;
                        }
                    }
                } else if (smoothedFps > 57.0f) {
                    if (appState_->render.pixelRatio < 1.0f) {
                        float pr = appState_->render.pixelRatio + 0.03f;
                        pr       = std::min(pr, 1.0f);
                        if (std::abs(pr - appState_->render.pixelRatio) > 1e-6f) {
                            appState_->render.pixelRatio = pr;
                            pixelRatioChanged            = true;
                            appState_->lod.lastDecision  = 3;
                        }
                    } else if (appState_->render.activeParticleCount < kParticleCountMax) {
                        uint32_t newCount =
                            static_cast<uint32_t>(static_cast<float>(appState_->render.activeParticleCount) * 1.05f);
                        newCount = std::min(newCount, kParticleCountMax);
                        if (newCount != appState_->render.activeParticleCount) {
                            appState_->render.activeParticleCount = newCount;
                            particleCountChanged                  = true;
                            appState_->lod.lastDecision           = 4;
                        }
                    }
                } else {
                    appState_->lod.lastDecision = 0;
                }

                if (particleCountChanged || pixelRatioChanged) {
                    appState_->render.densityComp =
                        ComputeDensityComp(appState_->render.activeParticleCount, appState_->render.pixelRatio);
                }
            }
        }

        // 将 UI/LOD 的 activeParticleCount 同步到后端实际渲染/Compute（particleCount_ + Indirect Args）。
        uint32_t desiredCount = appState_->render.activeParticleCount;
        desiredCount          = std::max(desiredCount, 1u);
        desiredCount          = std::min(desiredCount, kParticleCountMax);

        if (desiredCount < kParticleCountMin) {
            desiredCount = kParticleCountMin;
        }

        if (desiredCount != appState_->render.activeParticleCount) {
            appState_->render.activeParticleCount = desiredCount;
        }

        if (desiredCount != particleCount_) {
            particleCount_ = desiredCount;
            if (particleIndirectArgs_ != nullptr && immediateContext_ != nullptr) {
                // args = { NumVertices(6), NumInstances(particleCount_), StartVertex(0), FirstInstance(0) }
                immediateContext_->UpdateBuffer(particleIndirectArgs_, sizeof(uint32_t), sizeof(uint32_t),
                                                &particleCount_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
        }

        // OpenGL 版：当粒子数或 pixelRatio 发生变化时，重新推导 densityComp（保持亮度/遮蔽观感）。
        const bool basisChanged = (!lastLodBasisValid_) || desiredCount != prevBasisCount ||
                                  std::abs(appState_->render.pixelRatio - prevBasisPR) > 1e-6f;
        if (basisChanged) {
            appState_->render.densityComp = ComputeDensityComp(desiredCount, appState_->render.pixelRatio);
            lastLodParticleCount_         = desiredCount;
            lastLodPixelRatio_            = appState_->render.pixelRatio;
            lastLodBasisValid_            = true;
        }
    }

    // 更新崩溃诊断状态（用于 ErrorHandler 的崩溃报告/对话框）
    if (appState_ != nullptr) {
        totalFrameCount_++;
        bool handActive = false;
        if (handTracker_ && handTracker_->GetStatus() == HandTracking::Status::Ready) {
            handActive = handTracker_->GetLatestSample().hasHand;
        }
        ErrorHandler::UpdateState(totalFrameCount_, appState_->render.activeParticleCount, appState_->render.pixelRatio,
                                  handActive /*handTrackingActive*/);
    }

    // ImGui 新帧
    if (imgui_) {
        imgui_->NewFrame();

        // MD3 新帧
        MD3::BeginFrame(frameDt > 0.0f ? frameDt : (1.0f / 60.0f));
        MD3::SetDarkMode(appState_->ui.isDarkMode);
        MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));

        // 传递 Acrylic 合成纹理给 MD3（已包含：饱和度增强 + 近似 exclusion + tint 调制）
        MD3::SetBlurTexture(appState_->ui.enableBlur ? static_cast<void*>(uiAcrylicSRV_Strong_.RawPtr()) : nullptr,
                            appState_->ui.enableBlur);
        // 传递次级模糊纹理（用于折叠区域 Acrylic 效果，1/12 分辨率弱模糊）
        MD3::SetBlurTexture2(appState_->ui.enableBlur ? static_cast<void*>(uiAcrylicSRV_Weak_.RawPtr()) : nullptr);
        MD3::SetNoiseTexture(appState_->ui.enableBlur ? static_cast<void*>(uiNoiseSRV_.RawPtr()) : nullptr);
        MD3::SetNoiseIntensity((appState_ != nullptr) ? appState_->ui.noiseIntensity : 0.01f);

        // Error dialogs（统一错误处理）
        ErrorHandler::RenderErrorDialog(frameDt);

        // 崩溃分析器窗口（使用模糊背景）
        ImTextureID crashBlurTex =
            appState_->ui.enableBlur ? reinterpret_cast<ImTextureID>(uiAcrylicSRV_Strong_.RawPtr()) : 0;
        CrashAnalyzer::Render(appState_->ui.enableBlur, crashBlurTex, surfaceSize_.Width, surfaceSize_.Height,
                              appState_->ui.isDarkMode);

        // Debug 窗口（默认关闭，F3 切换）- 使用 MD3 无标题栏样式
        if (appState_ != nullptr && appState_->ui.showDebugWindow) {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(1200, 1200));
            constexpr ImGuiWindowFlags kDebugWindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
            ImGui::Begin("Debug", nullptr, kDebugWindowFlags);

            // 自定义标题栏
            constexpr float kTitleBarHeight = 40.0f;
            const auto&     str             = i18n::Get();
            MD3::WindowTitleBar(str.debugPanelTitle, &appState_->ui.showDebugWindow);

                // 绘制窗口背景
                {
                    ImVec2      pos   = ImGui::GetWindowPos();
                    ImVec2      size  = ImGui::GetWindowSize();
                    ImDrawList* dl    = ImGui::GetWindowDrawList();
                    ImGuiStyle& style = ImGui::GetStyle();

                    auto&  colors       = MD3::GetContext().colors;
                    auto&  ctx          = MD3::GetContext();
                    float  cornerRadius = style.WindowRounding;
                    ImVec2 endPos       = ImVec2(pos.x + size.x, pos.y + size.y);

                    // 模糊背景：如果启用且有有效纹理
                    const bool wantBlur = (appState_ != nullptr) ? appState_->ui.enableBlur : false;
                    ITextureView* blurSRV =
                        (wantBlur && uiAcrylicSRV_Strong_ != nullptr) ? uiAcrylicSRV_Strong_.RawPtr() : nullptr;
                    if (wantBlur) {
                        static bool s_warnedBlurSrvNull  = false;
                        static bool s_warnedNoiseSrvNull = false;
                        if (!s_warnedBlurSrvNull && uiAcrylicSRV_Strong_ == nullptr) {
                            OutputDebugStringA("[DiligentBackend] UI blur enabled but uiAcrylicSRV_Strong_ is null\n");
                            s_warnedBlurSrvNull = true;
                        }
                        if (!s_warnedNoiseSrvNull && uiNoiseSRV_ == nullptr) {
                            OutputDebugStringA("[DiligentBackend] UI blur enabled but uiNoiseSRV_ is null\n");
                            s_warnedNoiseSrvNull = true;
                        }
                    }

                    if (blurSRV != nullptr && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
                        // UV 计算：D3D12/Vulkan 纹理坐标系（Y 从上到下，无需翻转 Y）
                        ImVec2 uv0 = ImVec2(pos.x / ctx.screenWidth, pos.y / ctx.screenHeight);
                        ImVec2 uv1 = ImVec2(endPos.x / ctx.screenWidth, endPos.y / ctx.screenHeight);

                        // 使用带圆角的图片绘制，避免黑边
                        MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(blurSRV), pos, endPos, uv0, uv1,
                                             IM_COL32(255, 255, 255, 255), cornerRadius);

                        // 噪点层：防 banding + 增加“材质感”
                        if (wantBlur && uiNoiseSRV_ != nullptr) {
                            const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                            const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                            const ImU32 noiseCol  = IM_COL32(255, 255, 255, a);
                            MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(uiNoiseSRV_.RawPtr()), pos, endPos,
                                                 uv0, uv1, noiseCol, cornerRadius);
                        }

                        // 高光边框
                        ImU32 highlight =
                            appState_->ui.isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                    dl->AddRect(pos, endPos, highlight, cornerRadius, 0, 1.0f);
                } else {
                    // 无模糊时的纯色背景
                    ImVec4 bgCol = colors.surfaceContainerLow;
                    bgCol.w      = 0.95f;
                    dl->AddRectFilled(pos, endPos, ImGui::GetColorU32(bgCol), cornerRadius);
                }
            }

            // ========== 性能区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionPerformance, true)) {
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
                    ImGui::TextDisabled("%s", str.fps);
                    ImGui::TableNextColumn();
                    auto&  fpsColors = MD3::GetContext().colors;
                    ImVec4 fpsColor  = (currentFps_ >= 50.0f) ? fpsColors.primary
                                     : (currentFps_ >= 30.0f) ? fpsColors.tertiary
                                                              : fpsColors.error;
                    ImGui::TextColored(fpsColor, "%.1f", currentFps_);

                    const uint32_t uiParticleCount =
                        (appState_ != nullptr) ? appState_->render.activeParticleCount : particleCount_;
                    const float uiPixelRatio = (appState_ != nullptr) ? appState_->render.pixelRatio : 1.0f;
                    TwoColumnText(str.particles, "%u / %u", uiParticleCount, kParticleCountMax);
                    TwoColumnText(str.pixelRatio, "%.2f", uiPixelRatio);
                    TwoColumnText(str.resolution, "%u x %u", surfaceSize_.Width, surfaceSize_.Height);
                    TwoColumnText(str.backend, "%s", backend_ == Backend::D3D11 ? "D3D11" : backend_ == Backend::D3D12 ? "D3D12" : "Vulkan");

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

                // 绘图区域（调整右边距与左侧对齐 - 使用折叠区域的 contentPadding）
                float       contentIndent = 16.0f * appState_->ui.dpiScale;
                ImVec2      plotSize(ImGui::GetContentRegionAvail().x - contentIndent, 50);
                ImVec2      plotPos = ImGui::GetCursorScreenPos();
                ImVec2      plotEnd(plotPos.x + plotSize.x, plotPos.y + plotSize.y);
                ImDrawList* drawList     = ImGui::GetWindowDrawList();
                float       cornerRadius = 6.0f * appState_->ui.dpiScale;

                // 背景
                auto& ctx       = MD3::GetContext();
                auto& colors    = ctx.colors;
                ImU32 lineColor = ImGui::GetColorU32(colors.primary);
                ImU32 axisColor = IM_COL32(180, 180, 180, 140); // 半透明灰色用于 Y 轴标签

                // 如果启用模糊，绘制 Acrylic 效果背景
                if (ctx.blurEnabled && ctx.blurTextureID2 != nullptr && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
                    // UV 计算：D3D12/Vulkan 纹理坐标系（Y 从上到下）
                    ImVec2 uv0(plotPos.x / ctx.screenWidth, plotPos.y / ctx.screenHeight);
                    ImVec2 uv1(plotEnd.x / ctx.screenWidth, plotEnd.y / ctx.screenHeight);

                    // 弱模糊背景
                    MD3::AddImageRounded(drawList, reinterpret_cast<ImTextureID>(ctx.blurTextureID2), plotPos, plotEnd,
                                         uv0, uv1, IM_COL32(255, 255, 255, 255), cornerRadius);

                    // 噪点层：防 banding + 增加“材质感”
                    if (ctx.noiseTextureID != nullptr) {
                        const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                        const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                        const ImU32 noiseCol  = IM_COL32(255, 255, 255, a);
                        MD3::AddImageRounded(drawList, reinterpret_cast<ImTextureID>(ctx.noiseTextureID), plotPos,
                                             plotEnd, uv0, uv1, noiseCol, cornerRadius);
                    }

                    // 细边框
                    ImU32 borderColor = ctx.isDarkMode ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 20);
                    drawList->AddRect(plotPos, plotEnd, borderColor, cornerRadius, 0, 1.0f);
                } else {
                    // 无模糊时的纯色背景
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_FrameBg);
                    drawList->AddRectFilled(plotPos, plotEnd, bgColor, cornerRadius);
                }

                // 裁剪区域
                drawList->PushClipRect(plotPos, plotEnd, true);

                // 转换坐标（含滚动动画）
                // 计算滚动进度：使用 EaseOutCubic 缓动使动画开始快结束慢
                auto easeOutCubic = [](float t) -> float {
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
                };
                float scrollProgress = easeOutCubic(fpsGraphScrollAnimTime_ / kFpsHistorySampleInterval);

                auto toScreen = [&](int logicalIdx, float val) -> ImVec2 {
                    // 修复：最新数据点始终固定在右边界，滚动只影响较旧的点向左偏移
                    // normalizedX: 将逻辑索引 [0, N-1] 映射到 [0, 1]
                    float normalizedX = (float)logicalIdx / (float)(kFpsHistorySize - 1);
                    // scrollOffset: 新数据到来时从 0 渐变到 1/(N-1)，所有点同步左移
                    float scrollOffset = scrollProgress / (float)(kFpsHistorySize - 1);
                    float x            = plotPos.x + (normalizedX - scrollOffset) * plotSize.x;

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
                        dataPoints.push_back(toScreen(i, val));
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

                // Y 轴刻度（较小字体，半透明）
                char maxLabel[16], minLabel[16];
                snprintf(maxLabel, sizeof(maxLabel), "%.0f", maxVal);
                snprintf(minLabel, sizeof(minLabel), "%.0f", minVal);
                float smallFontSize = ImGui::GetFontSize() * 0.85f;
                drawList->AddText(ImGui::GetFont(), smallFontSize, ImVec2(plotPos.x + 4, plotPos.y + 2), axisColor,
                                  maxLabel);
                drawList->AddText(ImGui::GetFont(), smallFontSize,
                                  ImVec2(plotPos.x + 4, plotPos.y + plotSize.y - smallFontSize - 2), axisColor,
                                  minLabel);

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

            // ========== 手部追踪区域（HandTracker 集成）==========
            if (MD3::BeginCollapsingHeader(str.sectionHandTracking, true)) {
                HandTracking::Status st = HandTracking::Status::Unavailable;
                if (handTracker_ != nullptr) {
                    st = handTracker_->GetStatus();
                }

                const char* statusText = str.trackerUnavailable;
                ImVec4      statusCol  = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                switch (st) {
                case HandTracking::Status::NotStarted:
                    statusText = str.trackerNotStarted;
                    statusCol  = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    break;
                case HandTracking::Status::Starting:
                    statusText = str.trackerInitializing;
                    statusCol  = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                    break;
                case HandTracking::Status::Ready:
                    statusText = str.trackerReady;
                    statusCol  = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
                    break;
                case HandTracking::Status::Failed:
                    statusText = str.trackerFailed;
                    statusCol  = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    break;
                case HandTracking::Status::Unavailable:
                default:
                    statusText = str.trackerUnavailable;
                    statusCol  = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    break;
                }

                // 控制：重启并选择摄像头
                if (MD3::TonalButton(str.cameraSelectorButton)) {
                    if (handTracker_ != nullptr) {
                        handTracker_->RestartWithCameraSelector(true);
                    }
                }
                ImGui::TextDisabled("%s: #%d", str.selectedCamera,
                                    handTracker_ ? handTracker_->GetSelectedCamera() : -1);

                if (ImGui::BeginTable("TrackerTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.labelStatus);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(statusCol, "%s", statusText);

                    if (st == HandTracking::Status::Failed) {
                        const int errCode = handTracker_ ? handTracker_->GetLastErrorCode() : HANDTRACKER_ERROR_UNKNOWN;
                        const auto errMsg = handTracker_ ? handTracker_->GetLastErrorMessageUtf8() : std::string{};

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.labelErrorCode);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", errCode);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.labelErrorMessage);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", errMsg.empty() ? "-" : errMsg.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::Separator();

                // 用户可调参数：让它“真的生效”
                if (appState_ != nullptr) {
                    ImGui::Text("%s:", str.sensitivity);
                    MD3::Slider("##HandSensitivity", &appState_->handParams.sensitivity, 0.1f, 3.0f, "%.2f");
                    MD3::Toggle(str.invertX, &appState_->handParams.invertX);
                    MD3::Toggle(str.invertY, &appState_->handParams.invertY);

                    ImGui::Text("%s (%s):", str.handLostDelay, str.frames);
                    float delayF = static_cast<float>(appState_->handParams.handLostDelay);
                    if (MD3::Slider("##HandLostDelay", &delayF, 1.0f, 30.0f, "%.0f")) {
                        appState_->handParams.handLostDelay = static_cast<int>(delayF);
                    }
                }

                ImGui::Separator();

                // 追踪器调试开关
                bool debugEnabled = false;
                if (handTracker_ != nullptr && handTracker_->GetDebugMode(&debugEnabled)) {
                    if (MD3::Toggle(str.showCameraDebug, &debugEnabled)) {
                        handTracker_->SetDebugMode(debugEnabled);
                        if (appState_ != nullptr) {
                            appState_->ui.showCameraDebug = debugEnabled;
                        }
                    }
                } else {
                    ImGui::TextDisabled("%s: (%s)", str.showCameraDebug, str.notAvailable);
                }

                // SIMD mode
                ImGui::TextDisabled("%s:", str.simdMode);
                int simdMode = 0;
                if (handTracker_ != nullptr && handTracker_->GetSIMDMode(&simdMode)) {
                    const char* simdModes[] = {str.simdAuto, str.simdAVX2, str.simdSSE, str.simdScalar};
                    if (MD3::Combo(str.simdMode, &simdMode, simdModes, 4)) {
                        handTracker_->SetSIMDMode(simdMode);
                    }
                    const std::string impl = handTracker_->GetSIMDImplementation();
                    ImGui::Text("%s: %s", str.simdCurrent, impl.empty() ? str.statusUnknown : impl.c_str());
                } else {
                    ImGui::TextDisabled("%s: (%s)", str.simdMode, str.notAvailable);
                }

                ImGui::Separator();

                // 实时数值：raw vs smoothed（便于调试）
                HandTracking::Sample raw{};
                if (handTracker_ != nullptr && handTracker_->GetStatus() == HandTracking::Status::Ready) {
                    raw = handTracker_->GetLatestSample();
                }

                ImGui::TextDisabled("%s:", str.rawHandTrackerValues);
                if (ImGui::BeginTable("RawTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.handDetected);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", raw.hasHand ? str.yes : str.no);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.scale);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.scale);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotX);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.rotX);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotY);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.rotY);

                    ImGui::EndTable();
                }

                ImGui::Separator();

                ImGui::TextDisabled("%s:", str.smoothedAnimationValues);
                if (ImGui::BeginTable("AnimTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.scale);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animScale_);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotX);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animRotX_);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotY);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animRotY_);

                    ImGui::EndTable();
                }

                MD3::EndCollapsingHeader();
            }

            // ========== 视觉效果区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionVisuals)) {
                // 暗色模式切换 - 使用 MD3 Toggle
                if (MD3::Toggle(str.darkMode, &appState_->ui.isDarkMode)) {
                    // 应用 MD3 主题样式
                    MD3::ApplyImGuiStyle();
                }

                // Bloom 辉光效果开关
                MD3::Toggle(str.bloom, &bloomEnabled_);

                // 启用时显示强度滑块
                if (bloomEnabled_) {
                    ImGui::TextUnformatted(str.bloomStrength);
                    MD3::Slider("##BloomIntensity", &bloomStrength_, 0.1f, 1.5f, "%.2f");
                }

                ImGui::Spacing();

                // 玻璃模糊效果开关（窗口背景）
                MD3::Toggle(str.glassBlur, &appState_->ui.enableBlur);

                // 启用时显示强度滑块
                if (appState_->ui.enableBlur) {
                    ImGui::TextUnformatted(str.blurStrength);
                    MD3::Slider("##BlurStr", &appState_->ui.blurStrength, 0.0f, 5.0f, "%.1f");
                    ImGui::TextUnformatted(str.noise);
                    MD3::Slider("##Noise", &appState_->ui.noiseIntensity, 0.0f, 0.03f, "%.3f");
                }

                MD3::EndCollapsingHeader();
            }

            // ========== 窗口区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionWindow)) {
                // 图形后端切换
                ImGui::Text("%s:", str.switchBackend);
                int backendIndex = static_cast<int>(backend_);
                const char* backendNames[] = {"D3D11", "D3D12", "Vulkan"};
                if (MD3::Combo("##BackendSwitch", &backendIndex, backendNames, 3)) {
                    if (backendIndex != static_cast<int>(backend_)) {
                        // 用户选择了不同的后端，触发重启
                        auto newBackend = static_cast<Backend>(backendIndex);
                        if (Settings::RestartWithBackend(newBackend, *appState_)) {
                            PostQuitMessage(0);
                        }
                    }
                }
                ImGui::TextDisabled("%s", str.switchBackendConfirm);

                ImGui::Dummy(ImVec2(0, 5));

                // VSync 模式选择 - 使用 MD3 Combo
                ImGui::Text("%s:", str.vsync);
                int vsyncIndex = 1;
                if (appState_->render.vsyncMode == 0) {
                    vsyncIndex = 0;
                } else if (appState_->render.vsyncMode == 1) {
                    vsyncIndex = 1;
                } else {
                    vsyncIndex = 2; // -1 (Adaptive)
                }

                if (appState_->render.adaptiveVSyncSupported) {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn, str.vsyncAdaptive};
                    if (MD3::Combo("##VSync", &vsyncIndex, vsyncModes, 3)) {
                        appState_->render.vsyncMode = (vsyncIndex == 0) ? 0 : (vsyncIndex == 1) ? 1 : -1;
                    }
                } else {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn};
                    if (vsyncIndex > 1) {
                        vsyncIndex = 1; // 不支持 Adaptive 时回退到 On
                    }
                    if (MD3::Combo("##VSync", &vsyncIndex, vsyncModes, 2)) {
                        appState_->render.vsyncMode = vsyncIndex; // 0/1
                    }
                }

                ImGui::Dummy(ImVec2(0, 5));

                // Backdrop/透明合成开关（简化版：开=使用 Mica，关=Solid）
                // 注意：需要 Win10 1809+ 且窗口支持 DirectComposition；为保证运行期可反复切换不失效，DComp 一旦启用将保持启用。
                // 仅 D3D12 后端显示此选项（D3D11 可用 B 键切换）。
                if (appState_->backdrop.transparentSupported && backend_ == Backend::D3D12) {
                    bool transparent = appState_->backdrop.useTransparent;
                    if (MD3::Toggle(str.transparent, &transparent)) {
                        // 透明时使用 Mica (mode=3)，不透明时使用 Solid (mode=0)
                        const int newMode = transparent ? 3 : 0;
                        if (SetBackdropMode(newMode)) {
                            // 更新 backdropIndex 以匹配新模式
                            for (int i = 0; i < static_cast<int>(appState_->backdrop.availableBackdrops.size()); ++i) {
                                if (appState_->backdrop.availableBackdrops[i] == newMode) {
                                    appState_->backdrop.backdropIndex = i;
                                    break;
                                }
                            }
                        }
                    }
                }
                // Vulkan 后端或系统不支持时：不显示透明开关

                ImGui::Dummy(ImVec2(0, 5));

                // 显示状态
                ImGui::Text("%s: %s", str.fullscreen, appState_->window.isFullscreen ? str.yes : str.no);
                MD3::EndCollapsingHeader();
            }

            // ========== 高级区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionAdvanced)) {
                // 显示一些调试信息
                ImGui::TextDisabled("%s:", str.debugInfo);
                ImGui::Text("%s: %u", str.starCount, starCount_);
                ImGui::Text("%s: %u x %u", str.offscreen, surfaceSize_.Width, surfaceSize_.Height);

                ImGui::Dummy(ImVec2(0, 8));

                // 清除着色器缓存按钮
                static bool shaderCacheCleared = false;
                if (shaderCacheCleared) {
                    ImGui::BeginDisabled();
                    MD3::Button(str.shaderCacheCleared);
                    ImGui::EndDisabled();
                } else {
                    if (MD3::Button(str.clearShaderCache)) {
                        ClearShaderCache();
                        shaderCacheCleared = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", str.clearShaderCacheHint);
                    }
                }

                MD3::EndCollapsingHeader();
            }

            // ========== LOD 控制区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionLodControl)) {
                // 锁定 LOD 开关
                MD3::Toggle(str.lodLock, &appState_->lod.locked);

                ImGui::Dummy(ImVec2(0, 5));

                // 粒子数量滑块
                ImGui::Text("%s:", str.particleCount);
                float particleCount = static_cast<float>(appState_->render.activeParticleCount);
                if (MD3::Slider("##ParticleCount", &particleCount, static_cast<float>(kParticleCountMin),
                                static_cast<float>(kParticleCountMax), "%.0f")) {
                    appState_->render.activeParticleCount = static_cast<uint32_t>(particleCount);
                }

                ImGui::Dummy(ImVec2(0, 5));

                // 像素比例滑块
                ImGui::Text("%s:", str.pixelRatio);
                MD3::Slider("##PixelRatio", &appState_->render.pixelRatio, 0.5f, 1.0f, "%.2f");

                ImGui::Dummy(ImVec2(0, 5));

                // 密度补偿
                ImGui::Text("%s:", str.densityCompensation);
                MD3::Slider("##DensityComp", &appState_->render.densityComp, 0.0f, 2.0f, "%.2f");

                MD3::EndCollapsingHeader();
            }

            // ========== 日志区域 ==========
            // 复刻 OpenGL 版：级别过滤 + 搜索 + 暂停按钮（带图标）+ 清空/复制 + 日志列表
            if (MD3::BeginCollapsingHeader(str.sectionLog, true)) {
                static char logSearchBuffer[128] = "";
                static int  logLevelFilter       = 0; // 0=全部, 1=Info, 2=Warn, 3=Error

                float dpi           = appState_->ui.dpiScale;
                float controlHeight = 40.0f * dpi;   // 与 MD3::Combo 控件高度一致
                float buttonSize    = controlHeight; // 暂停按钮尺寸（正方形）

                // 第一行：级别过滤、搜索和暂停按钮
                ImGui::SetNextItemWidth(80 * dpi);
                const char* levelLabels[] = {str.logLevelAll, str.logLevelInfo, str.logLevelWarn, str.logLevelError};
                MD3::Combo("##LogLevel", &logLevelFilter, levelLabels, 4);

                ImGui::SameLine();
                // 搜索栏宽度 = 可用宽度 - 暂停按钮 - 间距 - 右侧留白
                float itemSpacingX   = ImGui::GetStyle().ItemSpacing.x;
                float rightMargin    = 12.0f * dpi; // 让暂停按钮不要贴右边界
                float minSearchWidth = 120.0f * dpi;
                float contentAvailX  = ImGui::GetContentRegionAvail().x;

                float searchWidth = contentAvailX - buttonSize - itemSpacingX - rightMargin;
                if (searchWidth < minSearchWidth) {
                    searchWidth = contentAvailX - buttonSize - itemSpacingX;
                }
                if (searchWidth < 1.0f) {
                    searchWidth = 1.0f;
                }

                ImGui::SetNextItemWidth(searchWidth);

                // InputText 走 ImGui 自带绘制：通过 FramePadding 精确对齐 MD3 的 40dp 高度，
                // 并用圆角裁剪避免文字“顶出”圆角区域
                float padY = (controlHeight - ImGui::GetFontSize()) * 0.5f;
                if (padY < 0.0f) {
                    padY = 0.0f;
                }

                float inputRounding = controlHeight * 0.5f;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f * dpi, padY));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, inputRounding);

                ImVec2 inputPos  = ImGui::GetCursorScreenPos();
                ImVec2 inputSize = ImVec2(searchWidth, controlHeight);
                MD3::PushRoundedClipRect(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y),
                                         inputRounding);
                ImGui::InputTextWithHint("##LogSearch", str.logSearch, logSearchBuffer, sizeof(logSearchBuffer));
                MD3::PopRoundedClipRect();

                ImGui::PopStyleVar(2);

                // 右键粘贴菜单
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                if (ImGui::BeginPopupContextItem("##LogSearchContext")) {
                    const char* clipText = ImGui::GetClipboardText();
                    bool        canPaste = (clipText && clipText[0] != '\0');
                    if (MD3::MenuItem(str.paste, canPaste, 36.0f * dpi)) {
                        // 追加粘贴内容到搜索栏
                        size_t currentLen = std::strlen(logSearchBuffer);
                        size_t clipLen    = std::strlen(clipText);
                        size_t maxAppend  = sizeof(logSearchBuffer) - 1 - currentLen;
                        if (clipLen > maxAppend) {
                            clipLen = maxAppend;
                        }
                        if (clipLen > 0) {
                            std::memcpy(logSearchBuffer + currentLen, clipText, clipLen);
                            logSearchBuffer[currentLen + clipLen] = '\0';
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar(2);

                // 暂停/继续按钮（MD3 风格按钮 + 图标）
                ImGui::SameLine();
                bool isPaused = DebugLog::Instance().IsPaused();

                ImGui::PushID("LogPauseBtn");
                ImGui::InvisibleButton("##btn", ImVec2(buttonSize, buttonSize));

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2      btnMin   = ImGui::GetItemRectMin();
                ImVec2      btnMax   = ImGui::GetItemRectMax();
                bool        hovered  = ImGui::IsItemHovered();
                bool        held     = ImGui::IsItemActive();
                bool        clicked  = ImGui::IsItemClicked(0);

                const MD3::MD3ColorScheme colors =
                    MD3::IsDarkMode() ? MD3::GetDarkColorScheme() : MD3::GetLightColorScheme();
                float rounding = std::min(12.0f * dpi, buttonSize * 0.5f);

                if (clicked) {
                    MD3::TriggerRippleForCurrentItem(ImGui::GetItemID(), rounding);
                    DebugLog::Instance().SetPaused(!isPaused);
                }

                if (hovered) {
                    ImGui::SetTooltip("%s", isPaused ? str.logResume : str.logPause);
                }

                // 轻微阴影（更接近 MD3 elevation）
                {
                    ImVec4 shadow = colors.shadow;
                    shadow.w      = 0.18f;
                    ImVec2 sMin(btnMin.x, btnMin.y + 1.0f * dpi);
                    ImVec2 sMax(btnMax.x, btnMax.y + 1.0f * dpi);
                    drawList->AddRectFilled(sMin, sMax, MD3::ColorToU32(shadow), rounding);
                }

                // 底色 + 状态层
                ImVec4 bgColor = colors.surfaceContainerHigh;
                if (hovered || held) {
                    float alpha = held ? colors.stateLayerPressed : colors.stateLayerHover;
                    bgColor     = MD3::ApplyStateLayer(bgColor, colors.onSurface, alpha);
                }
                drawList->AddRectFilled(btnMin, btnMax, MD3::ColorToU32(bgColor), rounding);

                // 图标：使用离线烘焙的 alpha 掩码（由原 SVG 转换得到），运行时只上传一次纹理
                float drawIconPx = 24.0f * dpi;
                if (auto* iconSRV = GetOrCreateLogControlIconSRV(isPaused); iconSRV != nullptr) {
                    float  cx = (btnMin.x + btnMax.x) * 0.5f;
                    float  cy = (btnMin.y + btnMax.y) * 0.5f;
                    ImVec2 iconMin(cx - drawIconPx * 0.5f, cy - drawIconPx * 0.5f);
                    ImVec2 iconMax(cx + drawIconPx * 0.5f, cy + drawIconPx * 0.5f);
                    drawList->AddImage(reinterpret_cast<ImTextureID>(iconSRV), iconMin, iconMax, ImVec2(0, 0),
                                       ImVec2(1, 1), IM_COL32_WHITE);
                }

                ImGui::PopID();

                // 第二行：清空和复制按钮
                if (MD3::TonalButton(str.clearLog)) {
                    DebugLog::Instance().Clear();
                }
                ImGui::SameLine();
                if (MD3::TonalButton(str.copyAllLog)) {
                    std::string filteredText = DebugLog::Instance().GetFilteredText(logSearchBuffer, logLevelFilter);
                    ImGui::SetClipboardText(filteredText.c_str());
                }

                // 日志列表（带过滤和搜索）
                DebugLog::Instance().Draw(logSearchBuffer, logLevelFilter);
                MD3::EndCollapsingHeader();
            }

            // 处理平滑滚动（必须在 WindowScrollbar 之前调用）
            MD3::HandleSmoothScroll(90.0f);

            // 自定义滚动条和缩放手柄
            MD3::WindowScrollbar(kTitleBarHeight);
            MD3::WindowResize(280.0f, 200.0f);

            ImGui::End();
        }

        // MD3 帧结束
        MD3::EndFrame();
    }

    // 先清屏 SwapChain（确保深度缓冲/RT 链路始终一致），再走离屏渲染 + 拷贝。
    // 1. Clear
    RenderClear();

    // 2. Offscreen Rendering (Compute + Stars + Particles)
    RenderOffscreen();

    // 3. Bloom（bright-pass + Kawase blur）
    RenderBloom();

    // 3.5 UI Blur：先把最终显示的场景颜色解析到中间纹理，再做低分辨率模糊
    // 注意：enableBlur=false 时必须整段跳过，否则会白跑大量 blur ping-pong pass。
    const bool wantUIBlur = (appState_ != nullptr) ? appState_->ui.enableBlur : true;
    if (wantUIBlur) {
        RenderUISceneForUI();
        RenderUIBlur();
        RenderAcrylicComposite();
    }

    // 4. Blit to Backbuffer
    BlitOffscreenToBackBuffer();

    // 渲染七段数码管 FPS（在 BlitOffscreenToBackBuffer 之后）
    RenderSevenSegmentFPS();

    // 渲染 ImGui（在七段数码管之后，Present 之前）
    if (imgui_) {
        ITextureView* pBackBufferRTV = GetCurrentBackBufferRTV();
        if (pBackBufferRTV != nullptr) {
            imgui_->Render(immediateContext_, pBackBufferRTV);
        }

        // 检查 ImGui 是否需要保存布局（用户移动/调整窗口后）
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantSaveIniSettings) {
            Settings::SaveImGuiLayout();
            io.WantSaveIniSettings = false;
        }
    }

    // 7. Present
    // VSync：
    // - 0  : Off  -> Present(0) -> Vulkan: MAILBOX/IMMEDIATE (无帧率限制)
    // - 1  : On   -> Present(1) -> Vulkan: FIFO (严格垂直同步)
    // - -1 : Adaptive -> Present(1) -> Vulkan: FIFO_RELAXED (自适应，帧晚则撕裂)
    // 注意：Diligent Vulkan 在 SyncInterval=1 时优先选择 FIFO_RELAXED，这正是自适应 VSync 的语义。
    int presentInterval = 1;
    if (appState_ != nullptr) {
        const int mode = appState_->render.vsyncMode;
        if (mode == 0) {
            presentInterval = 0;
        } else {
            // mode == 1 (On) 或 mode == -1 (Adaptive) 都使用 SyncInterval=1
            // Vulkan 会根据硬件支持选择 FIFO_RELAXED（自适应）或 FIFO（标准）
            presentInterval = 1;
        }
    }
    PresentFrame(presentInterval);
}

void DiligentBackend::RenderSevenSegmentFPS() {
    if (sevenSegPSO_ == nullptr || sevenSegSRB_ == nullptr || sevenSegConstants_ == nullptr) {
        return;
    }

    // 设置渲染目标为 SwapChain BackBuffer
    ITextureView* pRTV = GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }
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
        draw.Flags       = kDrawVerifyFlags;
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

// ============================================================================
// DirectComposition SwapChain 辅助方法
// ============================================================================

ITextureView* DiligentBackend::GetCurrentBackBufferRTV() {
    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        const Backend dcompBackend = dcompSwapChain_.GetBackendType();

        if (dcompBackend == Backend::D3D11) {
            // D3D11 模式：每帧动态重建当前后缓冲的 RTV
            // 因为 D3D11 + FLIP_SEQUENTIAL 不支持同时持有多个后缓冲的 RTV
            if (!UpdateD3D11CurrentBackBufferRTV()) {
                return nullptr;
            }
            return dcompBackBufferRTVs_[0].RawPtr();
        }

        // D3D12 模式：使用预创建的 RTV
        const uint32_t idx = dcompSwapChain_.GetCurrentBackBufferIndex();
        if (idx < kDCompBufferCount && dcompBackBufferRTVs_[idx]) {
            return dcompBackBufferRTVs_[idx].RawPtr();
        }
        return nullptr;
    }
    if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        return vkD3D12Interop_->GetSharedRTV();
    }
    if (swapChain_) {
        return swapChain_->GetCurrentBackBufferRTV();
    }
    return nullptr;
}

bool DiligentBackend::CreateDCompBackBufferRTVs() {
    OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs() called\n");
    if (!dcompSwapChain_.IsInitialized() || !device_) {
        OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs: dcompSwapChain_ or device_ not ready\n");
        return false;
    }

    const uint32_t bufferCount = dcompSwapChain_.GetBufferCount();
    const Backend  dcompBackend = dcompSwapChain_.GetBackendType();

    char dbgBuf[128];
    sprintf_s(dbgBuf, "[DiligentBackend] CreateDCompBackBufferRTVs: bufferCount=%u, backend=%d\n",
              bufferCount, static_cast<int>(dcompBackend));
    OutputDebugStringA(dbgBuf);

    if (dcompBackend == Backend::D3D12) {
        // D3D12 路径
        RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
        device_->QueryInterface(IID_RenderDeviceD3D12,
                                reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
        if (!deviceD3D12) {
            std::cerr << "[DiligentBackend] Failed to query IRenderDeviceD3D12" << std::endl;
            return false;
        }

        for (uint32_t i = 0; i < bufferCount && i < kDCompBufferCount; ++i) {
            ID3D12Resource* d3d12Resource = dcompSwapChain_.GetBackBufferD3D12(i);
            if (!d3d12Resource) {
                std::cerr << "[DiligentBackend] GetBackBufferD3D12(" << i << ") returned null" << std::endl;
                return false;
            }

            // 从 D3D12 资源创建 Diligent 纹理
            dcompBackBuffers_[i].Release();
            deviceD3D12->CreateTextureFromD3DResource(d3d12Resource, RESOURCE_STATE_PRESENT, &dcompBackBuffers_[i]);
            if (!dcompBackBuffers_[i]) {
                std::cerr << "[DiligentBackend] CreateTextureFromD3DResource failed for buffer " << i << std::endl;
                return false;
            }

            // 创建 RTV
            TextureViewDesc rtvDesc{};
            rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
            rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM_SRGB; // sRGB 视图
            dcompBackBufferRTVs_[i].Release();
            dcompBackBuffers_[i]->CreateView(rtvDesc, &dcompBackBufferRTVs_[i]);
            if (!dcompBackBufferRTVs_[i]) {
                std::cerr << "[DiligentBackend] CreateView RTV failed for buffer " << i << std::endl;
                return false;
            }
        }
    } else if (dcompBackend == Backend::D3D11) {
        // D3D11 路径
        // 注意：D3D11 + FLIP_SEQUENTIAL 模式下，不能同时为所有后缓冲创建 RTV
        // 只创建当前后缓冲的 RTV，每帧动态更新
        OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs: D3D11 path\n");
        RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
        device_->QueryInterface(IID_RenderDeviceD3D11,
                                reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
        if (!deviceD3D11) {
            OutputDebugStringA("[DiligentBackend] Failed to query IRenderDeviceD3D11\n");
            std::cerr << "[DiligentBackend] Failed to query IRenderDeviceD3D11" << std::endl;
            return false;
        }
        OutputDebugStringA("[DiligentBackend] IRenderDeviceD3D11 query OK\n");

        // D3D11 只创建当前后缓冲的纹理和 RTV
        const uint32_t currentIdx = dcompSwapChain_.GetCurrentBackBufferIndex();
        char dbgBuf[128];
        sprintf_s(dbgBuf, "[DiligentBackend] D3D11: Creating RTV for current buffer %u only\n", currentIdx);
        OutputDebugStringA(dbgBuf);

        ID3D11Texture2D* d3d11Texture = dcompSwapChain_.GetBackBufferD3D11(currentIdx);
        if (!d3d11Texture) {
            sprintf_s(dbgBuf, "[DiligentBackend] GetBackBufferD3D11(%u) returned null\n", currentIdx);
            OutputDebugStringA(dbgBuf);
            std::cerr << "[DiligentBackend] GetBackBufferD3D11(" << currentIdx << ") returned null" << std::endl;
            return false;
        }

        // 从 D3D11 资源创建 Diligent 纹理
        dcompBackBuffers_[0].Release();
        deviceD3D11->CreateTexture2DFromD3DResource(d3d11Texture, RESOURCE_STATE_RENDER_TARGET, &dcompBackBuffers_[0]);
        if (!dcompBackBuffers_[0]) {
            OutputDebugStringA("[DiligentBackend] CreateTexture2DFromD3DResource failed\n");
            std::cerr << "[DiligentBackend] CreateTexture2DFromD3DResource (D3D11) failed" << std::endl;
            return false;
        }

        // 创建 RTV
        // 注意：D3D11 不支持从 UNORM 纹理创建 SRGB 视图（D3D12 可以）
        // 所以 D3D11 必须使用 UNORM 格式，sRGB 校正需要在 shader 中手动处理
        TextureViewDesc rtvDesc{};
        rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
        rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM;
        dcompBackBufferRTVs_[0].Release();
        dcompBackBuffers_[0]->CreateView(rtvDesc, &dcompBackBufferRTVs_[0]);
        if (!dcompBackBufferRTVs_[0]) {
            OutputDebugStringA("[DiligentBackend] CreateView RTV (D3D11) failed\n");
            std::cerr << "[DiligentBackend] CreateView RTV (D3D11) failed" << std::endl;
            return false;
        }
        OutputDebugStringA("[DiligentBackend] D3D11 initial RTV created OK\n");
    } else {
        std::cerr << "[DiligentBackend] Unsupported DComp backend type" << std::endl;
        return false;
    }

    std::cout << "[DiligentBackend] Created " << bufferCount << " DComp back buffer RTVs ("
              << (dcompBackend == Backend::D3D11 ? "D3D11" : "D3D12") << ")" << std::endl;
    return true;
}

bool DiligentBackend::UpdateD3D11CurrentBackBufferRTV() {
    // D3D11 + FLIP 模式下，每帧需要重新调用 GetBuffer(0) 获取当前后缓冲
    // 不能使用缓存的后缓冲引用
    if (!dcompSwapChain_.IsInitialized() || !device_) {
        return false;
    }

    RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
    device_->QueryInterface(IID_RenderDeviceD3D11,
                            reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
    if (!deviceD3D11) {
        return false;
    }

    // D3D11 FLIP 模式：必须每帧调用 GetBuffer(0) 获取当前后缓冲
    IDXGISwapChain3* swapChain = dcompSwapChain_.GetSwapChain();
    if (!swapChain) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11Texture;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&d3d11Texture));
    if (FAILED(hr) || !d3d11Texture) {
        return false;
    }

    // 释放旧资源并重新创建
    dcompBackBuffers_[0].Release();
    dcompBackBufferRTVs_[0].Release();

    deviceD3D11->CreateTexture2DFromD3DResource(d3d11Texture.Get(), RESOURCE_STATE_RENDER_TARGET, &dcompBackBuffers_[0]);
    if (!dcompBackBuffers_[0]) {
        return false;
    }

    TextureViewDesc rtvDesc{};
    rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM; // D3D11 不支持从 UNORM 创建 SRGB 视图
    dcompBackBuffers_[0]->CreateView(rtvDesc, &dcompBackBufferRTVs_[0]);

    return dcompBackBufferRTVs_[0] != nullptr;
}

void DiligentBackend::PresentFrame(int syncInterval) {
    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        // DirectComposition 模式：需要手动 Flush + FinishFrame 来确保引擎正确回收资源
        immediateContext_->Flush();
        dcompSwapChain_.Present(static_cast<uint32_t>(syncInterval));
        // 通知引擎帧结束，回收动态描述符和过期资源
        immediateContext_->FinishFrame();
    } else if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        // Vulkan 互操作模式
        vkD3D12Interop_->FlushVulkan(); // 确保 Vulkan 渲染完成
        vkD3D12Interop_->Present(static_cast<uint32_t>(syncInterval));
        immediateContext_->FinishFrame();
    } else if (swapChain_) {
        // 标准模式：SwapChain->Present() 内部会调用 FinishFrame()
        swapChain_->Present(static_cast<Uint32>(syncInterval));
    }
}

bool DiligentBackend::SwitchTransparentMode(bool enableTransparent) {
    // 支持 D3D11 和 D3D12，Vulkan 暂不支持透明模式
    if (backend_ != Backend::D3D12 && backend_ != Backend::D3D11) {
        std::cerr << "[DiligentBackend] SwitchTransparentMode: backend not supported (D3D11/D3D12 only)" << std::endl;
        return false;
    }

    // 检查当前状态
    bool currentlyTransparent = false;
    if (backend_ == Backend::D3D12 || backend_ == Backend::D3D11) {
        currentlyTransparent = useDCompSwapChain_;
    } else if (backend_ == Backend::Vulkan) {
        currentlyTransparent = useVkD3D12Interop_;
    }

    if (currentlyTransparent == enableTransparent) {
        return true; // 无需切换
    }

    std::cout << "[DiligentBackend] Switching transparent mode: " << (enableTransparent ? "ON" : "OFF") << std::endl;

    // 1. 用深色/浅色清空当前帧，减少闪烁刺激
    const bool  isDarkMode = (appState_ != nullptr) ? appState_->ui.isDarkMode : true;
    const float clearR     = isDarkMode ? 0.0f : 1.0f;
    const float clearG     = isDarkMode ? 0.0f : 1.0f;
    const float clearB     = isDarkMode ? 0.0f : 1.0f;
    const float clearA     = 1.0f;

    {
        ITextureView* rtv = GetCurrentBackBufferRTV();
        if (rtv) {
            const float clearColor[] = {clearR, clearG, clearB, clearA};
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->ClearRenderTarget(rtv, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        PresentFrame(0); // 立即呈现，不等 VSync
    }

    // 2. 等待 GPU 完成所有工作
    if (backend_ == Backend::Vulkan && useVkD3D12Interop_ && vkD3D12Interop_) {
        vkD3D12Interop_->FlushVulkan();
    }
    immediateContext_->Flush();
    immediateContext_->FinishFrame();
    immediateContext_->WaitForIdle();

    // 3. 释放依赖 SwapChain 格式的 PSO 和绑定
    fullscreenQuadPSO_.Release();
    fullscreenQuadSRB_.Release();

    // 3.5 强制刷新窗口，清除 DWM 可能缓存的旧帧内容
    // 这对于从非透明切换到透明模式尤为重要
    if (enableTransparent && hwnd_) {
        // 使窗口无效并强制重绘
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
        // 给 DWM 一点时间刷新
        Sleep(10);
    }

    // 4. 销毁当前 SwapChain 资源
    if (backend_ == Backend::D3D12 || backend_ == Backend::D3D11) {
        if (useDCompSwapChain_) {
            // 释放 DComp 后缓冲
            for (auto& rtv : dcompBackBufferRTVs_) {
                rtv.Release();
            }
            for (auto& buf : dcompBackBuffers_) {
                buf.Release();
            }
            dcompSwapChain_.Shutdown();
            useDCompSwapChain_ = false;
        } else {
            swapChain_.Release();
        }
    } else if (backend_ == Backend::Vulkan) {
        if (useVkD3D12Interop_) {
            vkD3D12Interop_.reset();
            useVkD3D12Interop_ = false;
        } else {
            swapChain_.Release();
        }
    }

    // 4.5 窗口扩展样式（WS_EX_NOREDIRECTIONBITMAP）处理：
    // 该标志在本项目里用于 DirectComposition 透明 SwapChain。
    //
    // 注意：实际运行中把该标志“关掉再打开”会导致后续再开启系统 Backdrop（Mica/Acrylic）失效，
    // 现象为：DWM 仍显示纯色背景（用户侧观感：模糊彻底没了）。
    //
    // 因此这里改为“只确保开启，不主动关闭”，让窗口在支持平台上始终保持该标志，
    // 从而保证透明/Backdrop 可以稳定在运行期反复切换。
    if (enableTransparent && hwnd_) {
        const LONG_PTR exStyle  = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        const LONG_PTR desired  = exStyle | static_cast<LONG_PTR>(WS_EX_NOREDIRECTIONBITMAP);
        if (desired != exStyle) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, desired);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    // 5. 创建新的 SwapChain / Interop
    if (enableTransparent) {
        // 重要：在创建 DirectComposition 之前先设置 DWM backdrop（与启动时顺序一致）
        if (appState_ != nullptr && hwnd_ != nullptr) {
            const int micaMode = 3; // Mica
            ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd_, micaMode, *appState_);
            DwmFlush();
        }
        if (backend_ == Backend::D3D12) {
            // 切换到 DirectComposition 模式
            RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
            device_->QueryInterface(IID_RenderDeviceD3D12,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
            if (!deviceD3D12) {
                std::cerr << "[DiligentBackend] Failed to get IRenderDeviceD3D12" << std::endl;
                return false;
            }

            ID3D12Device* d3d12Device = deviceD3D12->GetD3D12Device();

            // 获取命令队列
            ICommandQueue* cmdQueueBase = immediateContext_->LockCommandQueue();
            if (!cmdQueueBase) {
                std::cerr << "[DiligentBackend] Failed to lock command queue" << std::endl;
                return false;
            }

            RefCntAutoPtr<ICommandQueueD3D12> cmdQueueD3D12;
            cmdQueueBase->QueryInterface(
                IID_CommandQueueD3D12, reinterpret_cast<IObject**>(static_cast<ICommandQueueD3D12**>(&cmdQueueD3D12)));
            if (!cmdQueueD3D12) {
                immediateContext_->UnlockCommandQueue();
                std::cerr << "[DiligentBackend] Failed to get ICommandQueueD3D12" << std::endl;
                return false;
            }

            ID3D12CommandQueue* cmdQueue = cmdQueueD3D12->GetD3D12CommandQueue();

            if (!dcompSwapChain_.InitD3D12(hwnd_, d3d12Device, cmdQueue, surfaceSize_.Width, surfaceSize_.Height,
                                      kDCompBufferCount)) {
                immediateContext_->UnlockCommandQueue();
                std::cerr << "[DiligentBackend] Failed to init DComp SwapChain" << std::endl;
                return false;
            }

            immediateContext_->UnlockCommandQueue();

            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to create DComp back buffer RTVs" << std::endl;
                return false;
            }

            useDCompSwapChain_ = true;
        } else if (backend_ == Backend::D3D11) {
            // D3D11 切换到 DirectComposition 模式
            RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
            device_->QueryInterface(IID_RenderDeviceD3D11,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
            if (!deviceD3D11) {
                std::cerr << "[DiligentBackend] Failed to get IRenderDeviceD3D11" << std::endl;
                return false;
            }

            ID3D11Device* d3d11Device = deviceD3D11->GetD3D11Device();

            if (!dcompSwapChain_.InitD3D11(hwnd_, d3d11Device, surfaceSize_.Width, surfaceSize_.Height,
                                           kDCompBufferCount)) {
                std::cerr << "[DiligentBackend] Failed to init D3D11 DComp SwapChain" << std::endl;
                return false;
            }

            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to create D3D11 DComp back buffer RTVs" << std::endl;
                return false;
            }

            useDCompSwapChain_ = true;
        } else if (backend_ == Backend::Vulkan) {
            // Vulkan 透明模式
            vkD3D12Interop_ = std::make_unique<VulkanD3D12Interop>();
            if (!vkD3D12Interop_->Init(hwnd_, device_, immediateContext_, surfaceSize_.Width, surfaceSize_.Height)) {
                std::cerr << "[DiligentBackend] Failed to init VulkanD3D12Interop" << std::endl;
                vkD3D12Interop_.reset();
                return false;
            }
            useVkD3D12Interop_ = true;
            // 同步 DWM
            DwmFlush();
        }
    } else {
        // 切换到标准 SwapChain 模式
        SwapChainDesc scDesc{};
        scDesc.Width             = surfaceSize_.Width;
        scDesc.Height            = surfaceSize_.Height;
        scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
        scDesc.BufferCount       = 3;
        scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;

        const NativeWindow window{reinterpret_cast<void*>(hwnd_)};

        if (backend_ == Backend::D3D12) {
            auto* factory = GetEngineFactoryD3D12();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get D3D12 factory" << std::endl;
                return false;
            }
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
            useDCompSwapChain_ = false;
        } else if (backend_ == Backend::D3D11) {
            auto* factory = GetEngineFactoryD3D11();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get D3D11 factory" << std::endl;
                return false;
            }
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D11(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
            useDCompSwapChain_ = false;
        } else if (backend_ == Backend::Vulkan) {
            auto* factory = GetEngineFactoryVk();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get Vulkan factory" << std::endl;
                return false;
            }
            factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);
            useVkD3D12Interop_ = false;
        }

        if (!swapChain_) {
            std::cerr << "[DiligentBackend] Failed to create standard SwapChain" << std::endl;
            return false;
        }
    }

    // 6. 重建依赖 SwapChain 格式的 PSO
    if (!CreateFullscreenQuadPSO()) {
        std::cerr << "[DiligentBackend] Failed to recreate fullscreen quad PSO" << std::endl;
        return false;
    }
    UpdateFullscreenQuadBindings();

    // 7. 切换后立即清空一帧（透明模式使用 alpha=0，不透明模式使用 alpha=1）
    {
        ITextureView* rtv = GetCurrentBackBufferRTV();
        if (rtv) {
            // 透明模式下需要 alpha=0，否则 DWM 会把内容当作不透明
            const float finalAlpha   = enableTransparent ? 0.0f : 1.0f;
            const float clearColor[] = {clearR, clearG, clearB, finalAlpha};
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->ClearRenderTarget(rtv, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        PresentFrame(0);
    }

    // 更新状态
    if (appState_ != nullptr) {
        appState_->backdrop.useTransparent = enableTransparent;
    }

    std::cout << "[DiligentBackend] Transparent mode switched successfully" << std::endl;
    return true;
}

} // namespace ParticleSaturn::Render
