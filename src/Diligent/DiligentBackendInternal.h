#pragma once

// ============================================================================
// DiligentBackend 拆分（D-015 后续 / cc8e4a）：原 DiligentBackend.cpp 匿名命名空间里
// 的文件局部结构体与自由辅助函数，上提到此私有内部头。巨物按功能拆到多个 TU
// （DiligentParticleSystem/DiligentBloom/DiligentUiComposite/DiligentStarField/
// DiligentSevenSegment/DiligentRenderTargets/DiligentPresent），它们共享这里的
// 常量/矩阵数学/着色器与 PSO 创建/粒子初始化，故集中到内部头，避免每个 TU 各自
// 重复定义导致漂移。自由函数一律 inline，常量一律 inline constexpr，满足跨 TU 的
// ODR。仅供 DiligentBackend 家族 TU include，不对外暴露。
// ============================================================================

#include "DiligentBackend.h"

#include "GraphicsTypes.h"
#include "PipelineState.h"
#include "RenderDevice.h"
#include "Shader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ParticleSaturn::Render::detail {

using namespace Diligent;

inline constexpr TEXTURE_FORMAT kOffscreenColorFormat = TEX_FORMAT_R11G11B10_FLOAT;

// Draw/DrawIndirect 验证开关：
// - Debug：启用验证，便于定位资源状态/参数错误
// - Release/FastRelease/Release_Static：全部关闭，避免开发期验证逻辑污染性能
//
// 注意：Diligent 自身在非 Development 构建下也会禁用验证，但这里仍显式置零，
// 保证“Release 系列”完全不走任何额外验证路径。
#if defined(NDEBUG)
inline constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_NONE;
#else
inline constexpr DRAW_FLAGS kDrawVerifyFlags = DRAW_FLAG_VERIFY_ALL;
#endif

// OpenGL 版星空策略：
// - 基准星数固定为 5 万（STAR_COUNT=50000）
// - 在低 pixelRatio 时绘制数量降到 60%（OpenGL：pixelRatio < 0.85）
inline constexpr uint32_t kStarCountBase = 50000u;
inline constexpr float    kStarLodRatio  = 0.6f;

inline constexpr float kDensityCompBase = 0.6f;

inline uint32_t HashFNV1a32Append(uint32_t hash, const char* s) {
    if (s == nullptr) {
        return hash;
    }
    while (*s) {
        hash ^= static_cast<uint8_t>(*s++);
        hash *= 16777619u;
    }
    return hash;
}

inline const char* GetBackendName(Backend backend) {
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

inline float ComputeDensityComp(uint32_t particleCount, float pixelRatio) {
    const float pr = (pixelRatio > 0.0f) ? pixelRatio : 1.0f;
    const float ratio = (DiligentBackend::kParticleCountMax > 0)
                            ? (static_cast<float>(particleCount) / static_cast<float>(DiligentBackend::kParticleCountMax))
                            : 1.0f;
    const float safeRatio = std::max(ratio, 0.0001f);
    return kDensityCompBase / std::pow(safeRatio, 0.7f) / std::pow(pr, 0.5f);
}

// Shader sources moved to DiligentShaderSources.cpp (reduce build time / file size).

inline RefCntAutoPtr<IShader> CreateShaderFromSource(IRenderDevice* device, const char* name, SHADER_TYPE type,
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

// 从预编译字节码创建着色器（无需运行时编译）
inline RefCntAutoPtr<IShader> CreateShaderFromBytecode(IRenderDevice* device, const char* name, SHADER_TYPE type,
                                                       const void* bytecode, size_t bytecodeSize) {
    ShaderCreateInfo shaderCI{};
    shaderCI.Desc.Name       = name;
    shaderCI.Desc.ShaderType = type;
    shaderCI.ByteCode        = bytecode;
    shaderCI.ByteCodeSize    = bytecodeSize;
    // 明确指定这是预编译字节码，让 Diligent 正确处理反射信息
    shaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_BYTECODE;

    RefCntAutoPtr<IShader> shader;
    device->CreateShader(shaderCI, &shader);
    return shader;
}

// 按后端选预编译字节码变体建 shader：Vulkan→SPIRV，D3D11/D3D12→DXBC。
// 用宏是因为两变体是不同符号（symBase##_SPIRV / _DXBC）且 sizeof 须作用于数组本体，
// 内联函数无法承载。调用点需已 `using namespace ShaderBytecodes;`。
#define PS_SHADER_FROM_BYTECODE(device, backend, name, shaderType, symBase)          \
    ((backend) == Backend::Vulkan                                                    \
         ? CreateShaderFromBytecode((device), (name), (shaderType), symBase##_SPIRV, \
                                    sizeof(symBase##_SPIRV))                          \
         : CreateShaderFromBytecode((device), (name), (shaderType), symBase##_DXBC,  \
                                    sizeof(symBase##_DXBC)))

// 创建 Graphics PSO 的辅助函数
inline void CreateGraphicsPSO(IRenderDevice* device, const GraphicsPipelineStateCreateInfo& psoCI,
                              IPipelineState** ppPSO) {
    *ppPSO = nullptr;
    device->CreateGraphicsPipelineState(psoCI, ppPSO);
}

// 创建 Compute PSO 的辅助函数
inline void CreateComputePSO(IRenderDevice* device, const ComputePipelineStateCreateInfo& psoCI,
                             IPipelineState** ppPSO) {
    *ppPSO = nullptr;
    device->CreateComputePipelineState(psoCI, ppPSO);
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

inline Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(Vec3 v) {
    const float len2 = Dot(v, v);
    if (len2 <= 0.0f) {
        return {0, 0, 0};
    }
    const float inv = 1.0f / std::sqrt(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

inline Mat4Rows LookAtRH(Vec3 eye, Vec3 center, Vec3 up) {
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

inline Mat4Rows PerspectiveRH_OpenGL(float fovyRad, float aspect, float zNear, float zFar) {
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

inline Mat4Rows RotationY(float a) {
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

inline Mat4Rows RotationX(float a) {
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

inline Mat4Rows RotationZ(float a) {
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

inline Mat4Rows Mul(Mat4Rows a, Mat4Rows b) {
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

    // Mesh Shader 需要的额外字段
    uint32_t ParticleCount = 0;
    uint32_t _pad[3]       = {0, 0, 0};
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

inline void HexToRGB(uint32_t rgb, float outColor[3]) {
    outColor[0] = HexToFloat((rgb >> 16) & 0xFF);
    outColor[1] = HexToFloat((rgb >> 8) & 0xFF);
    outColor[2] = HexToFloat((rgb >> 0) & 0xFF);
}

inline float Random01(uint32_t& state) {
    state           = state * 747796405u + 2891336453u;
    uint32_t result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result          = (result >> 22u) ^ result;
    return static_cast<float>(result) / 4294967295.0f;
}

inline uint32_t PackRGBA8(float r, float g, float b, float a) {
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

inline Vec3 HexToRGB(uint32_t hex) {
    return {HexToFloat((hex >> 16) & 0xFF), HexToFloat((hex >> 8) & 0xFF), HexToFloat(hex & 0xFF)};
}

inline Vec3 Mix(Vec3 a, Vec3 b, float t) {
    return {a.x * (1.0f - t) + b.x * t, a.y * (1.0f - t) + b.y * t, a.z * (1.0f - t) + b.z * t};
}

inline std::vector<SaturnParticle> InitSaturnParticlesCPU(uint32_t maxParticles, uint32_t seed) {
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
inline constexpr int kDigits[10][7] = {
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

} // namespace ParticleSaturn::Render::detail
