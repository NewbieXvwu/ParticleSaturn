// MD3Context.cpp - MD3 全局状态、初始化和帧管理
// 单一实现服务三种目标：
//   - macOS 全后端（OpenGL/Metal/Vulkan 回退，MD3_HAS_OPENGL）
//   - Windows OpenGL 目标（glad，MD3_HAS_OPENGL）
//   - Windows Diligent 目标（MD3_BACKEND_DILIGENT，无 OpenGL）

#if defined(MD3_BACKEND_DILIGENT)
// Diligent 后端：Ripple/Stencil 使用 Diligent PSO，不使用 OpenGL
#include "Buffer.h"
#include "DeviceContext.h"
#include "GraphicsTypes.h"
#include "ImGuiDiligent.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <OpenGL/gl3.h>
#define MD3_HAS_OPENGL 1
#endif
#elif defined(_WIN32)
#include <glad/glad.h>
#define MD3_HAS_OPENGL 1
#endif

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "MD3.h"

#if MD3_HAS_OPENGL
#include "MD3Shaders.h"
#endif

namespace MD3 {

// 全局上下文单例
static MD3Context g_context;

MD3Context& GetContext() {
    return g_context;
}

#if MD3_HAS_OPENGL
// 编译着色器（仅 OpenGL 后端）
static unsigned int CompileShader(unsigned int type, const char* source) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "[MD3] Shader compilation failed: " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// 创建着色器程序（仅 OpenGL 后端）
static unsigned int CreateProgram(const char* vertexSrc, const char* fragmentSrc) {
    unsigned int vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    unsigned int fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);

    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "[MD3] Program linking failed: " << infoLog << std::endl;
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}
#endif // MD3_HAS_OPENGL

#if defined(MD3_BACKEND_DILIGENT)
//=============================================================================
// Ripple HLSL Shaders
//=============================================================================

static const char* RippleVS = R"(
cbuffer Constants {
    float2 uScreenSize;
    float2 uRippleCenter;
    float  uRippleRadius;
    float  uRippleAlpha;
    float4 uRippleColor;
    float4 uBounds; // x, y, w, h
    float  uCornerRadius;
};

struct VSInput {
    float2 Pos : ATTRIB0;
};

struct PSInput {
    float4 Pos : SV_POSITION;
    float2 ScreenPos : TEXCOORD0;
};

void main(in VSInput VSIn, out PSInput PSIn) {
    PSIn.Pos = float4(VSIn.Pos, 0.0, 1.0);
    // 转换到屏幕坐标
    PSIn.ScreenPos = (VSIn.Pos * 0.5 + 0.5) * uScreenSize;
    PSIn.ScreenPos.y = uScreenSize.y - PSIn.ScreenPos.y; // Y 翻转
}
)";

static const char* RipplePS = R"(
cbuffer Constants {
    float2 uScreenSize;
    float2 uRippleCenter;
    float  uRippleRadius;
    float  uRippleAlpha;
    float4 uRippleColor;
    float4 uBounds; // x, y, w, h
    float  uCornerRadius;
};

struct PSInput {
    float4 Pos : SV_POSITION;
    float2 ScreenPos : TEXCOORD0;
};

// 圆角矩形 SDF
float roundedRectSDF(float2 p, float2 center, float2 halfSize, float radius) {
    float2 d = abs(p - center) - halfSize + radius;
    return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - radius;
}

float4 main(in PSInput PSIn) : SV_Target {
    float2 fragPos = PSIn.ScreenPos;

    float2 rectCenter = uBounds.xy + uBounds.zw * 0.5;
    float sdf = roundedRectSDF(fragPos, rectCenter, uBounds.zw * 0.5, uCornerRadius);
    float aa = 1.0 - smoothstep(0.0, 1.0, sdf);
    if (aa <= 0.0) discard;

    float dist = distance(fragPos, uRippleCenter);

    // 软边缘
    float edgeWidth = max(20.0, uRippleRadius * 0.15);
    float edge = 1.0 - smoothstep(uRippleRadius - edgeWidth, uRippleRadius, dist);

    // 中心淡出
    float fade = 1.0 - smoothstep(0.0, uRippleRadius, dist) * 0.3;

    return float4(uRippleColor.rgb, uRippleColor.a * uRippleAlpha * edge * fade * aa);
}
)";

// GLSL（Vulkan）版本：与 HLSL 常量/逻辑保持一致
static const char* RippleGlslVS = R"(
layout(std140, binding = 0) uniform Constants
{
    vec2  uScreenSize;
    vec2  uRippleCenter;
    float uRippleRadius;
    float uRippleAlpha;
    vec4  uRippleColor;
    vec4  uBounds; // x, y, w, h
    float uCornerRadius;
};

layout(location = 0) in vec2 inPos;
layout(location = 0) out vec2 vScreenPos;

void main()
{
    gl_Position = vec4(inPos, 0.0, 1.0);
    vScreenPos = (inPos * 0.5 + 0.5) * uScreenSize;
    vScreenPos.y = uScreenSize.y - vScreenPos.y;
}
)";

static const char* RippleGlslPS = R"(
layout(std140, binding = 0) uniform Constants
{
    vec2  uScreenSize;
    vec2  uRippleCenter;
    float uRippleRadius;
    float uRippleAlpha;
    vec4  uRippleColor;
    vec4  uBounds; // x, y, w, h
    float uCornerRadius;
};

layout(location = 0) in vec2 vScreenPos;
layout(location = 0) out vec4 oColor;

float roundedRectSDF(vec2 p, vec2 center, vec2 halfSize, float radius)
{
    vec2 d = abs(p - center) - halfSize + radius;
    return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0))) - radius;
}

void main()
{
    vec2 fragPos = vScreenPos;

    vec2 rectCenter = uBounds.xy + uBounds.zw * 0.5;
    float sdf = roundedRectSDF(fragPos, rectCenter, uBounds.zw * 0.5, uCornerRadius);
    float aa = 1.0 - smoothstep(0.0, 1.0, sdf);
    if (aa <= 0.0) discard;

    float dist = distance(fragPos, uRippleCenter);

    float edgeWidth = max(20.0, uRippleRadius * 0.15);
    float edge = 1.0 - smoothstep(uRippleRadius - edgeWidth, uRippleRadius, dist);

    float fade = 1.0 - smoothstep(0.0, uRippleRadius, dist) * 0.3;

    oColor = vec4(uRippleColor.rgb, uRippleColor.a * uRippleAlpha * edge * fade * aa);
}
)";

//=============================================================================
// Diligent 资源创建
//=============================================================================

static bool CreateRipplePSO(Diligent::IRenderDevice* device, ParticleSaturn::Render::Backend backend) {
    using namespace Diligent;

    auto& ctx = GetContext();

    // 创建 Vertex Shader
    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage  = (backend == ParticleSaturn::Render::Backend::Vulkan) ? SHADER_SOURCE_LANGUAGE_GLSL
                                                                                    : SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
    shaderCI.Desc.Name       = "MD3 Ripple VS";
    shaderCI.Source          = (backend == ParticleSaturn::Render::Backend::Vulkan) ? RippleGlslVS : RippleVS;
    shaderCI.EntryPoint      = "main";

    RefCntAutoPtr<IShader> pVS;
    device->CreateShader(shaderCI, &pVS);
    if (!pVS) {
        std::cerr << "[MD3] Failed to create Ripple VS" << std::endl;
        return false;
    }

    // 创建 Pixel Shader
    shaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    shaderCI.Desc.Name       = "MD3 Ripple PS";
    shaderCI.Source          = (backend == ParticleSaturn::Render::Backend::Vulkan) ? RippleGlslPS : RipplePS;

    RefCntAutoPtr<IShader> pPS;
    device->CreateShader(shaderCI, &pPS);
    if (!pPS) {
        std::cerr << "[MD3] Failed to create Ripple PS" << std::endl;
        return false;
    }

    // 创建 PSO
    GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name = "MD3 Ripple PSO";
    psoCI.pVS          = pVS;
    psoCI.pPS          = pPS;

    // 输入布局
    LayoutElement layoutElems[] = {
        {0, 0, 2, VT_FLOAT32, False} // Position
    };
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;
    psoCI.GraphicsPipeline.InputLayout.NumElements    = 1;

    // 图元类型
    psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    // 渲染目标格式（与 swap chain 一致）
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = TEX_FORMAT_RGBA8_UNORM_SRGB;
    // 与 ImGuiDiligent 的 DSV 保持一致（用于 stencil 链路；Ripple 自身不做 depth test）
    psoCI.GraphicsPipeline.DSVFormat = TEX_FORMAT_D24_UNORM_S8_UINT;

    // 混合状态（alpha blending）
    auto& RT0          = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    RT0.BlendEnable    = True;
    RT0.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
    RT0.DestBlend      = BLEND_FACTOR_INV_SRC_ALPHA;
    RT0.BlendOp        = BLEND_OPERATION_ADD;
    RT0.SrcBlendAlpha  = BLEND_FACTOR_ONE;
    RT0.DestBlendAlpha = BLEND_FACTOR_INV_SRC_ALPHA;
    RT0.BlendOpAlpha   = BLEND_OPERATION_ADD;

    // 光栅化状态
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode = CULL_MODE_NONE;

    // 深度模板状态
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // 资源布局：常量缓冲作为 STATIC 变量（每帧只更新 buffer 内容即可）
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "Constants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "Constants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.Variables    = vars;
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);

    RefCntAutoPtr<IPipelineState> pPSO;
    device->CreateGraphicsPipelineState(psoCI, &pPSO);
    if (!pPSO) {
        std::cerr << "[MD3] Failed to create Ripple PSO" << std::endl;
        return false;
    }

    ctx.diligent.ripplePSO = pPSO.Detach();

    // 创建 Constants buffer
    BufferDesc cbDesc;
    cbDesc.Name           = "MD3 Ripple Constants";
    cbDesc.Size           = 256; // 对齐到 256，避免不同后端/驱动对 CB 对齐要求差异
    cbDesc.Usage          = USAGE_DYNAMIC;
    cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

    RefCntAutoPtr<IBuffer> pCB;
    device->CreateBuffer(cbDesc, nullptr, &pCB);
    ctx.diligent.rippleConstants = pCB.Detach();

    // 绑定常量缓冲到 STATIC 变量（PSO 生命周期内固定）
    if (auto* varVS = ctx.diligent.ripplePSO->GetStaticVariableByName(SHADER_TYPE_VERTEX, "Constants");
        varVS != nullptr) {
        varVS->Set(ctx.diligent.rippleConstants);
    }
    if (auto* varPS = ctx.diligent.ripplePSO->GetStaticVariableByName(SHADER_TYPE_PIXEL, "Constants");
        varPS != nullptr) {
        varPS->Set(ctx.diligent.rippleConstants);
    }

    // 创建 SRB（无额外资源，但需要用于 CommitShaderResources）
    RefCntAutoPtr<IShaderResourceBinding> pSRB;
    ctx.diligent.ripplePSO->CreateShaderResourceBinding(&pSRB, true);
    ctx.diligent.rippleSRB = pSRB.Detach();

    // 创建全屏四边形 VB
    float      quadVerts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    BufferDesc vbDesc;
    vbDesc.Name      = "MD3 Ripple VB";
    vbDesc.Size      = sizeof(quadVerts);
    vbDesc.BindFlags = BIND_VERTEX_BUFFER;
    vbDesc.Usage     = USAGE_IMMUTABLE;

    BufferData vbData;
    vbData.pData    = quadVerts;
    vbData.DataSize = sizeof(quadVerts);

    RefCntAutoPtr<IBuffer> pVB;
    device->CreateBuffer(vbDesc, &vbData, &pVB);
    ctx.diligent.rippleVB = pVB.Detach();

    std::cout << "[MD3] Ripple PSO created successfully" << std::endl;
    return true;
}
#endif // MD3_BACKEND_DILIGENT

#if defined(MD3_BACKEND_DILIGENT)
void Init(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, ParticleSaturn::Render::Backend backend,
          float dpiScale) {
    auto& ctx = GetContext();
    if (ctx.initialized) {
        return;
    }

    ctx.dpiScale    = dpiScale;
    ctx.isDarkMode  = true;
    ctx.colors      = GetDarkColorScheme();
    ctx.frameIndex  = 0;
    ctx.currentTime = 0.0f;
    ctx.deltaTime   = 0.0f;

    // 设置 ImGui 圆形细分精度，减少描边断裂
    ImGui::GetStyle().CircleTessellationMaxError = 0.1f;

    // 保存 Diligent 设备引用
    ctx.diligent.device  = device;
    ctx.diligent.context = context;

    // 创建 Ripple 渲染资源
    if (device && context) {
        ctx.diligent.initialized = CreateRipplePSO(device, backend);
    }

    ctx.initialized = true;
    std::cout << "[MD3] Material Design 3 UI system initialized (Diligent)" << std::endl;
}
#else
void Init(float dpiScale, bool useOpenGL) {
    if (g_context.initialized) {
        return;
    }

    g_context.dpiScale   = dpiScale;
    g_context.isDarkMode = true;
    g_context.colors     = GetDarkColorScheme();
    g_context.useOpenGL  = useOpenGL;

#if MD3_HAS_OPENGL
    if (useOpenGL) {
        // 创建 Ripple 着色器程序
        g_context.rippleProgram = CreateProgram(MD3Shaders::VertexRipple, MD3Shaders::FragmentRipple);

        if (!g_context.rippleProgram) {
            std::cerr << "[MD3] Failed to create ripple shader program" << std::endl;
        }

        // 创建全屏四边形 VAO/VBO
        float quadVerts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

        glGenVertexArrays(1, &g_context.rippleVAO);
        glGenBuffers(1, &g_context.rippleVBO);

        glBindVertexArray(g_context.rippleVAO);
        glBindBuffer(GL_ARRAY_BUFFER, g_context.rippleVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glBindVertexArray(0);
    }
#else
    (void)useOpenGL; // 避免未使用警告
#endif

    g_context.initialized = true;
    std::cout << "[MD3] Material Design 3 UI system initialized (OpenGL: " << (useOpenGL ? "yes" : "no") << ")"
              << std::endl;
}
#endif // MD3_BACKEND_DILIGENT (Init selection)

void Shutdown() {
    if (!g_context.initialized) {
        return;
    }

#if defined(MD3_BACKEND_DILIGENT)
    // 释放 Diligent 资源
    if (g_context.diligent.ripplePSO) {
        g_context.diligent.ripplePSO->Release();
        g_context.diligent.ripplePSO = nullptr;
    }
    if (g_context.diligent.rippleSRB) {
        g_context.diligent.rippleSRB->Release();
        g_context.diligent.rippleSRB = nullptr;
    }
    if (g_context.diligent.rippleConstants) {
        g_context.diligent.rippleConstants->Release();
        g_context.diligent.rippleConstants = nullptr;
    }
    if (g_context.diligent.rippleVB) {
        g_context.diligent.rippleVB->Release();
        g_context.diligent.rippleVB = nullptr;
    }
    g_context.diligent.device      = nullptr;
    g_context.diligent.context     = nullptr;
    g_context.diligent.initialized = false;
#endif

#if MD3_HAS_OPENGL
    if (g_context.rippleProgram) {
        glDeleteProgram(g_context.rippleProgram);
        g_context.rippleProgram = 0;
    }

    if (g_context.rippleVAO) {
        glDeleteVertexArrays(1, &g_context.rippleVAO);
        g_context.rippleVAO = 0;
    }

    if (g_context.rippleVBO) {
        glDeleteBuffers(1, &g_context.rippleVBO);
        g_context.rippleVBO = 0;
    }
#endif

    g_context.ripples.clear();
    g_context.toggleStates.clear();
    g_context.buttonStates.clear();
    g_context.sliderStates.clear();
    g_context.cardStates.clear();
    g_context.comboStates.clear();
    g_context.selectableStates.clear();
    g_context.collapsingHeaderStates.clear();
    g_context.windowStates.clear();
    g_context.scrollbarStates.clear();
    g_context.resizeStates.clear();
    g_context.smoothScrollStates.clear();

    g_context.initialized = false;
    std::cout << "[MD3] Material Design 3 UI system shutdown" << std::endl;
}

void BeginFrame(float dt) {
#if defined(MD3_BACKEND_DILIGENT)
    if (!g_context.initialized) {
        return;
    }
#endif
    g_context.deltaTime = dt;
    g_context.currentTime += dt;
    g_context.frameIndex++;

    // 更新所有 Ripple 动画
    auto&       ripples = g_context.ripples;
    const auto& config  = g_context.rippleConfig;

    for (auto it = ripples.begin(); it != ripples.end();) {
        RippleState& r = *it;
        r.time += dt;

        if (!r.fadeOut) {
            // 扩散阶段
            float expandProgress = r.time / config.expandDuration;
            if (expandProgress >= 1.0f) {
                expandProgress = 1.0f;
                r.fadeOut      = true;
                r.time         = 0.0f; // 重置时间用于淡出
            }

            // 使用 ease-out 曲线
            float eased = 1.0f - (1.0f - expandProgress) * (1.0f - expandProgress);
            r.radius    = r.maxRadius * eased;
            r.alpha     = config.maxAlpha;
        } else {
            // 淡出阶段
            float fadeProgress = r.time / config.fadeDuration;
            if (fadeProgress >= 1.0f) {
                it = ripples.erase(it);
                continue;
            }

            r.radius = r.maxRadius;
            r.alpha  = config.maxAlpha * (1.0f - fadeProgress);
        }

        ++it;
    }

    // 更新所有控件动画状态
    for (auto& [id, state] : g_context.toggleStates) {
        state.knobPosition.Update(dt);
        state.trackFill.Update(dt);
        state.knobScale.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.buttonStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
        state.pressState.Update(dt);
    }

    for (auto& [id, state] : g_context.sliderStates) {
        state.thumbScale.Update(dt);
        state.activeTrack.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.cardStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.comboStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    int expectedFrameSeen = g_context.frameIndex - 1;
    for (auto& [id, state] : g_context.selectableStates) {
        if (state.lastFrameSeen != expectedFrameSeen) {
            state.hoverState.target = 0.0f;
        }
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.collapsingHeaderStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    for (auto& [id, state] : g_context.windowStates) {
        state.closeButtonHover.Update(dt);
        state.closeButtonPress.Update(dt);
        // 窗口生命周期动画
        state.openProgress.Update(dt);
        state.scale.Update(dt);
        state.offsetY.Update(dt);
        state.alpha.Update(dt);
    }

    for (auto& [id, state] : g_context.scrollbarStates) {
        state.hoverState.Update(dt);
        state.dragState.Update(dt);
        state.visibility.Update(dt);
        // 更新隐藏计时器
        if (state.hideTimer > 0.0f) {
            state.hideTimer -= dt;
            if (state.hideTimer <= 0.0f) {
                state.visibility.target = 0.0f;
            }
        }
    }

    for (auto& [id, state] : g_context.resizeStates) {
        state.hoverState.Update(dt);
    }

#if defined(MD3_BACKEND_DILIGENT)
    for (auto& [id, state] : g_context.smoothScrollStates) {
        state.scrollY.Update(dt);
    }
#endif
}

void EndFrame() {
#if defined(MD3_BACKEND_DILIGENT)
    // 渲染所有活跃的 Ripple 效果
    DrawRipplesDiligent();
#endif
}

void SetDarkMode(bool dark) {
    if (g_context.isDarkMode == dark) {
        return;
    }

    g_context.isDarkMode = dark;
    g_context.colors     = dark ? GetDarkColorScheme() : GetLightColorScheme();

    std::cout << "[MD3] Theme changed to: " << (dark ? "Dark" : "Light") << std::endl;
}

bool IsDarkMode() {
    return g_context.isDarkMode;
}

void SetScreenSize(float width, float height) {
    g_context.screenWidth  = width;
    g_context.screenHeight = height;
}

void SetDpiScale(float scale) {
#if defined(MD3_BACKEND_DILIGENT)
    if (std::abs(g_context.dpiScale - scale) < 0.001f) {
        return; // 无变化
    }
    g_context.dpiScale = scale;
    // DPI 变化时重新应用 ImGui 样式（圆角、间距等需要按 DPI 缩放）
    ApplyImGuiStyle();
    // 设置 ImGui 全局字体缩放（用于运行时 DPI 变化，无需重建字体纹理）
    ImGui::GetIO().FontGlobalScale = scale;
#else
    g_context.dpiScale = scale;
#endif
}

#if defined(MD3_BACKEND_DILIGENT)
void SetBlurTexture(void* textureID, bool enabled) {
    g_context.blurTextureID = textureID;
    g_context.blurEnabled   = enabled;
}

void SetBlurTexture2(void* textureID) {
    g_context.blurTextureID2 = textureID;
}

void SetNoiseTexture(void* textureID) {
    g_context.noiseTextureID = textureID;
}
#else
void SetBlurTexture(unsigned int textureID, bool enabled) {
    g_context.blurTextureID = textureID;
    g_context.blurEnabled   = enabled;
}

void SetBlurTexture2(unsigned int textureID) {
    g_context.blurTextureID2 = textureID;
}

void SetNoiseTexture(unsigned int textureID) {
    g_context.noiseTextureID = textureID;
}
#endif

void SetNoiseIntensity(float intensity) {
    g_context.noiseIntensity = intensity;
}

//=============================================================================
// Ripple API 实现
//=============================================================================

struct RippleDrawData {
    float centerX;
    float centerY;
    float radius;
    float alpha;
    float boundsX;
    float boundsY;
    float boundsW;
    float boundsH;
    float cornerRadius;
    float colorR;
    float colorG;
    float colorB;
    float screenW;
    float screenH;
};

#if MD3_HAS_OPENGL
static void DrawRippleShaderCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const RippleDrawData* data = static_cast<const RippleDrawData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    if (!g_context.rippleProgram || !g_context.rippleVAO) {
        ImGui::MemFree(cmd->UserCallbackData);
        return;
    }

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);

    glUseProgram(g_context.rippleProgram);

    const int uRippleCenter = glGetUniformLocation(g_context.rippleProgram, "uRippleCenter");
    const int uRippleRadius = glGetUniformLocation(g_context.rippleProgram, "uRippleRadius");
    const int uRippleAlpha  = glGetUniformLocation(g_context.rippleProgram, "uRippleAlpha");
    const int uRippleColor  = glGetUniformLocation(g_context.rippleProgram, "uRippleColor");
    const int uBounds       = glGetUniformLocation(g_context.rippleProgram, "uBounds");
    const int uCornerRadius = glGetUniformLocation(g_context.rippleProgram, "uCornerRadius");
    const int uScreenSize   = glGetUniformLocation(g_context.rippleProgram, "uScreenSize");

    if (uRippleCenter >= 0) {
        glUniform2f(uRippleCenter, data->centerX, data->centerY);
    }
    if (uRippleRadius >= 0) {
        glUniform1f(uRippleRadius, data->radius);
    }
    if (uRippleAlpha >= 0) {
        glUniform1f(uRippleAlpha, data->alpha);
    }
    if (uRippleColor >= 0) {
        glUniform4f(uRippleColor, data->colorR, data->colorG, data->colorB, 1.0f);
    }
    if (uBounds >= 0) {
        glUniform4f(uBounds, data->boundsX, data->boundsY, data->boundsW, data->boundsH);
    }
    if (uCornerRadius >= 0) {
        glUniform1f(uCornerRadius, data->cornerRadius);
    }
    if (uScreenSize >= 0) {
        glUniform2f(uScreenSize, data->screenW, data->screenH);
    }

    glBindVertexArray(g_context.rippleVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glUseProgram(0);

    ImGui::MemFree(cmd->UserCallbackData);
}
#endif // MD3_HAS_OPENGL

void TriggerRipple(ImGuiID id, float centerX, float centerY, float boundsX, float boundsY, float boundsW, float boundsH,
                   float cornerRadius) {
    // 计算最大半径（覆盖整个控件对角线）
    float dx1 = centerX - boundsX;
    float dx2 = (boundsX + boundsW) - centerX;
    float dy1 = centerY - boundsY;
    float dy2 = (boundsY + boundsH) - centerY;

    float maxDx     = std::max(dx1, dx2);
    float maxDy     = std::max(dy1, dy2);
    float maxRadius = std::sqrt(maxDx * maxDx + maxDy * maxDy);

    // 获取 Ripple 颜色
    const auto& colors      = g_context.colors;
    ImVec4      rippleColor = g_context.isDarkMode ? colors.onSurface : colors.primary;

    // 获取当前窗口信息
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    RippleState state;
    state.widgetId = id;
    // 存储相对于控件的点击位置
    state.relCenterX   = centerX - boundsX;
    state.relCenterY   = centerY - boundsY;
    state.radius       = 0.0f;
    state.maxRadius    = maxRadius;
    state.alpha        = 0.0f;
    state.time         = 0.0f;
    state.boundsW      = boundsW;
    state.boundsH      = boundsH;
    state.cornerRadius = cornerRadius;
    state.colorR       = rippleColor.x;
    state.colorG       = rippleColor.y;
    state.colorB       = rippleColor.z;
    state.colorA       = 1.0f;
    // 存储窗口信息用于滚动补偿
    if (window) {
        state.windowId          = window->ID;
        state.initialWindowPosX = window->Pos.x;
        state.initialWindowPosY = window->Pos.y;
        state.initialScrollX    = window->Scroll.x;
        state.initialScrollY    = window->Scroll.y;
    }
    state.initialBoundsX = boundsX;
    state.initialBoundsY = boundsY;
    state.active         = true;
    state.fadeOut        = false;

    g_context.ripples.push_back(state);
}

void TriggerRippleForCurrentItem(ImGuiID id, float cornerRadius) {
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    ImVec2 itemMin  = ImGui::GetItemRectMin();
    ImVec2 itemMax  = ImGui::GetItemRectMax();

    TriggerRipple(id, mousePos.x, mousePos.y, itemMin.x, itemMin.y, itemMax.x - itemMin.x, itemMax.y - itemMin.y,
                  cornerRadius);
}

#if !defined(MD3_BACKEND_DILIGENT)
void DrawRipples() {
    if (g_context.ripples.empty()) {
        return;
    }

    const ImGuiIO& io       = ImGui::GetIO();
    const float    fbScaleX = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
    const float    fbScaleY = (io.DisplayFramebufferScale.y > 0.0f) ? io.DisplayFramebufferScale.y : 1.0f;
    const float    fbScaleR = std::max(fbScaleX, fbScaleY);

    // 获取当前窗口
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) {
        return;
    }

    ImDrawList* dl        = ImGui::GetWindowDrawList();
    const bool  useShader = (g_context.rippleProgram != 0 && g_context.rippleVAO != 0);

    // 绘制属于当前窗口的所有 Ripple
    for (const auto& r : g_context.ripples) {
        if (r.alpha <= 0.001f) {
            continue;
        }
        if (r.windowId != window->ID) {
            continue; // 只绘制当前窗口的 ripple
        }

        // 计算滚动偏移量
        float scrollDeltaX = window->Scroll.x - r.initialScrollX;
        float scrollDeltaY = window->Scroll.y - r.initialScrollY;

        // 动态获取控件当前边界（解决控件尺寸变化后 Ripple 仍按旧尺寸渲染的问题）
        float boundsW        = r.boundsW;
        float boundsH        = r.boundsH;
        float cornerRadius   = r.cornerRadius;
        float currentBoundsX = r.initialBoundsX - scrollDeltaX;
        float currentBoundsY = r.initialBoundsY - scrollDeltaY;

        auto it = g_context.widgetBounds.find(r.widgetId);
        if (it != g_context.widgetBounds.end()) {
            const auto& wb = it->second;
            // widgetBounds 中的坐标已经是当前帧的屏幕坐标，不需要再补偿滚动
            currentBoundsX = wb.x;
            currentBoundsY = wb.y;
            boundsW        = wb.w;
            boundsH        = wb.h;
            cornerRadius   = wb.cornerRadius;
        }

        // 计算 ripple 中心的屏幕位置
        float centerX = currentBoundsX + r.relCenterX;
        float centerY = currentBoundsY + r.relCenterY;

        // 保存裁剪区域（先用矩形裁剪把像素工作量限定在控件区域内）
        ImVec2 clipMin(currentBoundsX, currentBoundsY);
        ImVec2 clipMax(currentBoundsX + boundsW, currentBoundsY + boundsH);
        dl->PushClipRect(clipMin, clipMax, true);

        if (useShader) {
#if MD3_HAS_OPENGL
            RippleDrawData* data = static_cast<RippleDrawData*>(ImGui::MemAlloc(sizeof(RippleDrawData)));
            // Shader 使用 gl_FragCoord（Framebuffer 像素坐标），所以这里必须把 ImGui 坐标转换到 FB 像素坐标。
            data->centerX      = centerX * fbScaleX;
            data->centerY      = centerY * fbScaleY;
            data->radius       = r.radius * fbScaleR;
            data->alpha        = r.alpha;
            data->boundsX      = currentBoundsX * fbScaleX;
            data->boundsY      = currentBoundsY * fbScaleY;
            data->boundsW      = boundsW * fbScaleX;
            data->boundsH      = boundsH * fbScaleY;
            data->cornerRadius = cornerRadius * fbScaleR;
            data->colorR       = r.colorR;
            data->colorG       = r.colorG;
            data->colorB       = r.colorB;
            data->screenW      = io.DisplaySize.x * fbScaleX;
            data->screenH      = io.DisplaySize.y * fbScaleY;

            dl->AddCallback(DrawRippleShaderCallback, data);
            dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
#endif
        } else {
            // 纯 ImGui 回退（Metal/Vulkan/无 OpenGL 路径）
            ImVec4 rippleColor(r.colorR, r.colorG, r.colorB, r.alpha);
            ImU32  col = ColorToU32(rippleColor);
            dl->AddCircleFilled(ImVec2(centerX, centerY), r.radius, col, 64);
        }

        dl->PopClipRect();
    }
}
#else  // MD3_BACKEND_DILIGENT

void DrawRipples() {
    auto& ctx = GetContext();
    if (ctx.ripples.empty()) {
        return;
    }

    // Use foreground draw list - works outside of window context
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) {
        return;
    }

    // 绘制属于当前窗口的所有 Ripple（使用 ImDrawList 实现）
    for (const auto& r : ctx.ripples) {
        if (r.alpha <= 0.001f) {
            continue;
        }
        // Get the window by ID to check scroll offset
        ImGuiWindow* window = ImGui::FindWindowByID(r.windowId);

        float scrollDeltaX = 0.0f;
        float scrollDeltaY = 0.0f;

        if (window) {
            scrollDeltaX = window->Scroll.x - r.initialScrollX;
            scrollDeltaY = window->Scroll.y - r.initialScrollY;
        }

        // 动态获取控件当前边界（解决控件尺寸变化后 Ripple 仍按旧尺寸渲染的问题）
        float boundsW        = r.boundsW;
        float boundsH        = r.boundsH;
        float cornerRadius   = r.cornerRadius;
        float currentBoundsX = r.initialBoundsX - scrollDeltaX;
        float currentBoundsY = r.initialBoundsY - scrollDeltaY;

        auto it = ctx.widgetBounds.find(r.widgetId);
        if (it != ctx.widgetBounds.end()) {
            const auto& wb = it->second;
            // widgetBounds 中的坐标已经是当前帧的屏幕坐标，不需要再补偿滚动
            currentBoundsX = wb.x;
            currentBoundsY = wb.y;
            boundsW        = wb.w;
            boundsH        = wb.h;
            cornerRadius   = wb.cornerRadius;
        }

        // 计算 ripple 中心的屏幕位置
        float centerX = currentBoundsX + r.relCenterX;
        float centerY = currentBoundsY + r.relCenterY;

        // 获取窗口裁剪区域，将控件边界与窗口可见区域取交集
        ImVec2 windowClipMin(0, 0);
        ImVec2 windowClipMax(ctx.screenWidth, ctx.screenHeight);
        if (window) {
            windowClipMin = window->ClipRect.Min;
            windowClipMax = window->ClipRect.Max;
        }

        // Push clip rect for the control bounds intersected with window clip rect
        ImVec2 boundsMin(std::max(currentBoundsX, windowClipMin.x), std::max(currentBoundsY, windowClipMin.y));
        ImVec2 boundsMax(std::min(currentBoundsX + boundsW, windowClipMax.x),
                         std::min(currentBoundsY + boundsH, windowClipMax.y));

        // 如果裁剪区域无效（完全在窗口外），跳过此 ripple
        if (boundsMax.x <= boundsMin.x || boundsMax.y <= boundsMin.y) {
            continue;
        }

        dl->PushClipRect(boundsMin, boundsMax, true);

        // Render the ripple with rounded corner awareness
        ImVec4 rippleColor(r.colorR, r.colorG, r.colorB, r.alpha);
        ImU32  col = ColorToU32(rippleColor);

        if (cornerRadius > 1.0f) {
            // For rounded corners: draw ripple as intersection of circle and rounded rect
            float left   = std::max(boundsMin.x, centerX - r.radius);
            float right  = std::min(boundsMax.x, centerX + r.radius);
            float top    = std::max(boundsMin.y, centerY - r.radius);
            float bottom = std::min(boundsMax.y, centerY + r.radius);

            if (right > left && bottom > top) {
                ImVec2 rippleMin(left, top);
                ImVec2 rippleMax(right, bottom);

                // Calculate effective corner radius for the intersection region
                float effectiveCorner = std::min(cornerRadius, std::min((right - left) * 0.5f, (bottom - top) * 0.5f));

                // Draw as rounded rectangle to respect the control's corner radius
                dl->AddRectFilled(rippleMin, rippleMax, col, effectiveCorner);
            }
        } else {
            // No corner radius - simple circle fill
            dl->AddCircleFilled(ImVec2(centerX, centerY), r.radius, col, 64);
        }

        dl->PopClipRect();
    }
}

//=============================================================================
// Ripple（Diligent 渲染路径：通过 ImDrawList callback 在 ImGuiDiligent::Render() 中执行）
//=============================================================================

static void DrawRippleDiligentCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    using namespace Diligent;

    auto* data = static_cast<RippleDrawData*>(cmd->UserCallbackData);
    if (data == nullptr) {
        return;
    }

    auto& ctx = GetContext();
    if (!ctx.diligent.initialized || ctx.diligent.ripplePSO == nullptr || ctx.diligent.rippleSRB == nullptr ||
        ctx.diligent.rippleConstants == nullptr || ctx.diligent.rippleVB == nullptr) {
        ImGui::MemFree(data);
        return;
    }

    auto*           imgui = ParticleSaturn::UI::GetImGuiDiligentInstance();
    IDeviceContext* dc    = (imgui != nullptr) ? imgui->GetCurrentContext() : ctx.diligent.context;
    if (dc == nullptr) {
        ImGui::MemFree(data);
        return;
    }

    // 设置 scissor（ImGui 的 callback 分支不会自动设置裁剪矩形）
    if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr) {
        const ImVec2 clipOff   = drawData->DisplayPos;
        const ImVec2 clipScale = drawData->FramebufferScale;

        ImVec2 clipMin((cmd->ClipRect.x - clipOff.x) * clipScale.x, (cmd->ClipRect.y - clipOff.y) * clipScale.y);
        ImVec2 clipMax((cmd->ClipRect.z - clipOff.x) * clipScale.x, (cmd->ClipRect.w - clipOff.y) * clipScale.y);

        const float  vpWf = drawData->DisplaySize.x * clipScale.x;
        const float  vpHf = drawData->DisplaySize.y * clipScale.y;
        const Uint32 vpW  = vpWf > 0.0f ? static_cast<Uint32>(vpWf) : 0;
        const Uint32 vpH  = vpHf > 0.0f ? static_cast<Uint32>(vpHf) : 0;

        if (vpW > 0 && vpH > 0) {
            auto clampI32 = [](float v, float lo, float hi) -> Int32 {
                if (v < lo) {
                    v = lo;
                }
                if (v > hi) {
                    v = hi;
                }
                return static_cast<Int32>(v);
            };

            Rect scissor{};
            scissor.left   = clampI32(clipMin.x, 0.0f, static_cast<float>(vpW));
            scissor.top    = clampI32(clipMin.y, 0.0f, static_cast<float>(vpH));
            scissor.right  = clampI32(clipMax.x, 0.0f, static_cast<float>(vpW));
            scissor.bottom = clampI32(clipMax.y, 0.0f, static_cast<float>(vpH));
            dc->SetScissorRects(1, &scissor, vpW, vpH);
        }
    }

    // 更新常量缓冲（HLSL/GLSL 共享同一字段顺序）
    struct RippleCB {
        float uScreenSize[2];
        float uRippleCenter[2];
        float uRippleRadius;
        float uRippleAlpha;
        float pad0[2];
        float uRippleColor[4];
        float uBounds[4];
        float uCornerRadius;
        float pad1[3];
    };

    PVoid mapped = nullptr;
    dc->MapBuffer(ctx.diligent.rippleConstants, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
    if (mapped != nullptr) {
        auto* cb             = static_cast<RippleCB*>(mapped);
        cb->uScreenSize[0]   = data->screenW;
        cb->uScreenSize[1]   = data->screenH;
        cb->uRippleCenter[0] = data->centerX;
        cb->uRippleCenter[1] = data->centerY;
        cb->uRippleRadius    = data->radius;
        cb->uRippleAlpha     = data->alpha;
        cb->pad0[0]          = 0.0f;
        cb->pad0[1]          = 0.0f;
        cb->uRippleColor[0]  = data->colorR;
        cb->uRippleColor[1]  = data->colorG;
        cb->uRippleColor[2]  = data->colorB;
        cb->uRippleColor[3]  = 1.0f;
        cb->uBounds[0]       = data->boundsX;
        cb->uBounds[1]       = data->boundsY;
        cb->uBounds[2]       = data->boundsW;
        cb->uBounds[3]       = data->boundsH;
        cb->uCornerRadius    = data->cornerRadius;
        cb->pad1[0] = cb->pad1[1] = cb->pad1[2] = 0.0f;
        dc->UnmapBuffer(ctx.diligent.rippleConstants, MAP_WRITE);
    }

    dc->SetPipelineState(ctx.diligent.ripplePSO);
    dc->CommitShaderResources(ctx.diligent.rippleSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    IBuffer* vb     = ctx.diligent.rippleVB;
    Uint64   offset = 0;
    dc->SetVertexBuffers(0, 1, &vb, &offset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, SET_VERTEX_BUFFERS_FLAG_RESET);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = DRAW_FLAG_VERIFY_ALL;
    dc->Draw(draw);

    ImGui::MemFree(data);
}

void DrawRipplesDiligent() {
    auto& ctx = GetContext();
    if (ctx.ripples.empty()) {
        return;
    }

    // 如果 Diligent PSO 未初始化，使用 ImDrawList 回退
    if (!ctx.diligent.initialized || ctx.diligent.ripplePSO == nullptr) {
        DrawRipples();
        return;
    }

    // 使用 ForegroundDrawList - 可在任何上下文中工作
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) {
        DrawRipples();
        return;
    }

    const ImGuiIO& io       = ImGui::GetIO();
    const float    fbScaleX = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
    const float    fbScaleY = (io.DisplayFramebufferScale.y > 0.0f) ? io.DisplayFramebufferScale.y : 1.0f;
    const float    fbScaleR = std::max(fbScaleX, fbScaleY);

    for (const auto& r : ctx.ripples) {
        if (r.alpha <= 0.001f) {
            continue;
        }

        // 使用 FindWindowByID 获取窗口（不依赖当前窗口上下文）
        ImGuiWindow* window = ImGui::FindWindowByID(r.windowId);

        float scrollDeltaX = 0.0f;
        float scrollDeltaY = 0.0f;

        if (window) {
            scrollDeltaX = window->Scroll.x - r.initialScrollX;
            scrollDeltaY = window->Scroll.y - r.initialScrollY;
        }

        // 动态获取控件当前边界（解决控件尺寸变化后 Ripple 仍按旧尺寸渲染的问题）
        float boundsW        = r.boundsW;
        float boundsH        = r.boundsH;
        float cornerRadius   = r.cornerRadius;
        float currentBoundsX = r.initialBoundsX - scrollDeltaX;
        float currentBoundsY = r.initialBoundsY - scrollDeltaY;

        auto it = ctx.widgetBounds.find(r.widgetId);
        if (it != ctx.widgetBounds.end()) {
            const auto& wb = it->second;
            // widgetBounds 中的坐标已经是当前帧的屏幕坐标，不需要再补偿滚动
            currentBoundsX = wb.x;
            currentBoundsY = wb.y;
            boundsW        = wb.w;
            boundsH        = wb.h;
            cornerRadius   = wb.cornerRadius;
        }

        const float centerX = currentBoundsX + r.relCenterX;
        const float centerY = currentBoundsY + r.relCenterY;

        // 获取窗口裁剪区域，将控件边界与窗口可见区域取交集
        ImVec2 windowClipMin(0, 0);
        ImVec2 windowClipMax(io.DisplaySize.x, io.DisplaySize.y);
        if (window) {
            windowClipMin = window->ClipRect.Min;
            windowClipMax = window->ClipRect.Max;
        }

        // 用矩形裁剪限制像素工作量（与窗口裁剪区域取交集）
        const ImVec2 clipMin(std::max(currentBoundsX, windowClipMin.x), std::max(currentBoundsY, windowClipMin.y));
        const ImVec2 clipMax(std::min(currentBoundsX + boundsW, windowClipMax.x),
                             std::min(currentBoundsY + boundsH, windowClipMax.y));

        // 如果裁剪区域无效（完全在窗口外），跳过此 ripple
        if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
            continue;
        }

        dl->PushClipRect(clipMin, clipMax, true);

        // 分配回调数据
        auto* data         = static_cast<RippleDrawData*>(ImGui::MemAlloc(sizeof(RippleDrawData)));
        data->centerX      = centerX * fbScaleX;
        data->centerY      = centerY * fbScaleY;
        data->radius       = r.radius * fbScaleR;
        data->alpha        = r.alpha;
        data->boundsX      = currentBoundsX * fbScaleX;
        data->boundsY      = currentBoundsY * fbScaleY;
        data->boundsW      = boundsW * fbScaleX;
        data->boundsH      = boundsH * fbScaleY;
        data->cornerRadius = cornerRadius * fbScaleR;
        data->colorR       = r.colorR;
        data->colorG       = r.colorG;
        data->colorB       = r.colorB;
        data->screenW      = io.DisplaySize.x * fbScaleX;
        data->screenH      = io.DisplaySize.y * fbScaleY;

        // 添加 Diligent PSO 渲染回调
        dl->AddCallback(DrawRippleDiligentCallback, data);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

        dl->PopClipRect();
    }
}
#endif // MD3_BACKEND_DILIGENT

//=============================================================================
// 工具函数实现
//=============================================================================

//=============================================================================
// 圆角裁剪（stencil）
//=============================================================================

struct RoundedClipBeginData {
    int prevRef;
    int newRef;
};

struct RoundedClipEndData {
    int  ref;
    bool disable;
};

static std::vector<int> s_roundedClipStack;
static int              s_roundedClipRef = 0;

#if MD3_HAS_OPENGL
static void RoundedClipBeginCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const RoundedClipBeginData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    if (data->prevRef == 0) {
        glStencilFunc(GL_ALWAYS, data->newRef, 0xFF);
    } else {
        glStencilFunc(GL_EQUAL, data->prevRef, 0xFF);
    }

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    ImGui::MemFree(cmd->UserCallbackData);
}

static void RoundedClipEndCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const RoundedClipEndData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    if (data->disable) {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        ImGui::MemFree(cmd->UserCallbackData);
        return;
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, data->ref, 0xFF);

    ImGui::MemFree(cmd->UserCallbackData);
}
#endif // MD3_HAS_OPENGL

#if defined(MD3_BACKEND_DILIGENT)
// Stencil 写入回调：设置 Stencil 写入模式（递增）
static void RoundedClipBeginCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* data = static_cast<RoundedClipBeginData*>(cmd->UserCallbackData);
    if (data == nullptr) {
        return;
    }

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui != nullptr) {
        imgui->SetStencilMode(StencilMode::WriteIncr, data->newRef);
    }

    ImGui::MemFree(data);
}

// Stencil 测试回调：切换到 Stencil 测试模式
static void RoundedClipTestCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* data = static_cast<RoundedClipBeginData*>(cmd->UserCallbackData);
    if (data == nullptr) {
        return;
    }

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui != nullptr) {
        imgui->SetStencilMode(StencilMode::TestEqual, data->newRef);
    }

    ImGui::MemFree(data);
}

// 恢复 Stencil 状态回调
static void RoundedClipEndCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* data = static_cast<RoundedClipEndData*>(cmd->UserCallbackData);
    if (data == nullptr) {
        return;
    }

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui != nullptr) {
        if (data->disable) {
            imgui->SetStencilMode(StencilMode::Disabled, 0);
        } else {
            imgui->SetStencilMode(StencilMode::TestEqual, data->ref);
        }
    }

    ImGui::MemFree(data);
}
#endif // MD3_BACKEND_DILIGENT

void PushRoundedClipRect(const ImVec2& clip_min, const ImVec2& clip_max, float rounding) {
#if defined(MD3_BACKEND_DILIGENT)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    // 保存当前参考值，递增新参考值
    int prevRef = s_roundedClipRef;
    s_roundedClipRef++;
    int newRef = s_roundedClipRef;

    // 压入栈
    s_roundedClipStack.push_back(newRef);

    // 设置矩形裁剪（作为基础裁剪）
    dl->PushClipRect(clip_min, clip_max, true);

    // 分配回调数据（Begin）
    auto* beginData    = static_cast<RoundedClipBeginData*>(ImGui::MemAlloc(sizeof(RoundedClipBeginData)));
    beginData->prevRef = prevRef;
    beginData->newRef  = newRef;
    dl->AddCallback(RoundedClipBeginCallback, beginData);

    // 绘制圆角矩形到 Stencil（颜色不重要，因为不写入颜色缓冲）
    dl->AddRectFilled(clip_min, clip_max, IM_COL32_WHITE, rounding);

    // 重置渲染状态（确保 PSO 正确切换）
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    // 分配回调数据（Test）
    auto* testData    = static_cast<RoundedClipBeginData*>(ImGui::MemAlloc(sizeof(RoundedClipBeginData)));
    testData->prevRef = prevRef;
    testData->newRef  = newRef;
    dl->AddCallback(RoundedClipTestCallback, testData);
#elif MD3_HAS_OPENGL
    if (!g_context.useOpenGL) {
        // 非 OpenGL 后端：使用普通裁剪
        ImGui::GetWindowDrawList()->PushClipRect(clip_min, clip_max, true);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    int prevRef = s_roundedClipRef;
    int newRef  = prevRef + 1;
    if (newRef <= 0 || newRef > 255) {
        s_roundedClipRef = 0;
        s_roundedClipStack.clear();
        prevRef = 0;
        newRef  = 1;
    }

    s_roundedClipStack.push_back(prevRef);
    s_roundedClipRef = newRef;

    auto* begin    = static_cast<RoundedClipBeginData*>(ImGui::MemAlloc(sizeof(RoundedClipBeginData)));
    begin->prevRef = prevRef;
    begin->newRef  = newRef;
    dl->AddCallback(RoundedClipBeginCallback, begin);

    ImVec4 dummy(1.0f, 1.0f, 1.0f, 1.0f);
    dl->AddRectFilled(clip_min, clip_max, ColorToU32(dummy), rounding);

    auto* end    = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
    end->ref     = newRef;
    end->disable = false;
    dl->AddCallback(RoundedClipEndCallback, end);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
#else
    // 非 OpenGL 后端：使用普通裁剪
    (void)rounding; // 避免未使用警告
    ImGui::GetWindowDrawList()->PushClipRect(clip_min, clip_max, true);
#endif
}

void PopRoundedClipRect() {
#if defined(MD3_BACKEND_DILIGENT)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    if (s_roundedClipStack.empty()) {
        return;
    }

    // 弹出当前层
    s_roundedClipStack.pop_back();

    // 计算恢复的参考值
    int  restoreRef = s_roundedClipStack.empty() ? 0 : s_roundedClipStack.back();
    bool disable    = (restoreRef == 0);

    s_roundedClipRef = restoreRef;

    auto* endData    = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
    endData->ref     = restoreRef;
    endData->disable = disable;
    dl->AddCallback(RoundedClipEndCallback, endData);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    dl->PopClipRect();
#elif MD3_HAS_OPENGL
    if (!g_context.useOpenGL) {
        // 非 OpenGL 后端：使用普通裁剪
        ImGui::GetWindowDrawList()->PopClipRect();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    if (s_roundedClipStack.empty()) {
        s_roundedClipRef = 0;
        auto* end        = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
        end->ref         = 0;
        end->disable     = true;
        dl->AddCallback(RoundedClipEndCallback, end);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        return;
    }

    int prevRef = s_roundedClipStack.back();
    s_roundedClipStack.pop_back();
    s_roundedClipRef = prevRef;

    auto* end    = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
    end->ref     = prevRef;
    end->disable = (prevRef == 0);
    dl->AddCallback(RoundedClipEndCallback, end);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
#else
    // 非 OpenGL 后端：使用普通裁剪
    ImGui::GetWindowDrawList()->PopClipRect();
#endif
}

ImVec4 BlendColors(const ImVec4& base, const ImVec4& overlay, float alpha) {
    return ImVec4(base.x + (overlay.x - base.x) * alpha, base.y + (overlay.y - base.y) * alpha,
                  base.z + (overlay.z - base.z) * alpha, base.w + (overlay.w - base.w) * alpha);
}

ImVec4 ApplyStateLayer(const ImVec4& base, const ImVec4& stateColor, float stateAlpha) {
    // 正确的状态层混合：在基础颜色上叠加半透明状态颜色
    return ImVec4(base.x * (1.0f - stateAlpha) + stateColor.x * stateAlpha,
                  base.y * (1.0f - stateAlpha) + stateColor.y * stateAlpha,
                  base.z * (1.0f - stateAlpha) + stateColor.z * stateAlpha, base.w);
}

unsigned int ColorToU32(const ImVec4& color) {
    unsigned int r = (unsigned int)(color.x * 255.0f);
    unsigned int g = (unsigned int)(color.y * 255.0f);
    unsigned int b = (unsigned int)(color.z * 255.0f);
    unsigned int a = (unsigned int)(color.w * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

ImVec4 HexToColor(unsigned int hex, float alpha) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

#if defined(MD3_BACKEND_DILIGENT)
void ApplyImGuiStyle() {
    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiStyle& style      = ImGui::GetStyle();
    ImVec4*     imguiColor = style.Colors;

    // 圆角设置
    style.WindowRounding    = 14.0f * dpi;
    style.ChildRounding     = 16.0f * dpi;
    style.FrameRounding     = 20.0f * dpi;
    style.PopupRounding     = 20.0f * dpi;
    style.ScrollbarRounding = 12.0f * dpi;
    style.GrabRounding      = 20.0f * dpi;
    style.TabRounding       = 12.0f * dpi;

    // 间距设置
    style.WindowPadding    = ImVec2(20.0f * dpi, 20.0f * dpi);
    style.FramePadding     = ImVec2(10.0f * dpi, 6.0f * dpi);
    style.ItemSpacing      = ImVec2(10.0f * dpi, 12.0f * dpi);
    style.ItemInnerSpacing = ImVec2(8.0f * dpi, 6.0f * dpi);
    style.ScrollbarSize    = 12.0f * dpi;
    style.GrabMinSize      = 12.0f * dpi;

    // 边框
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 0.0f;
    style.PopupBorderSize  = 1.0f;
    style.FrameBorderSize  = 0.0f;

    // 应用 MD3 颜色到 ImGui 样式
    // 窗口背景 - 设置为透明，让自定义绘制的模糊背景显示
    ImVec4 windowBg               = colors.surface;
    windowBg.w                    = 0.0f; // 完全透明，背景由自定义绘制处理
    imguiColor[ImGuiCol_WindowBg] = windowBg;

    // 子窗口/Card 背景
    ImVec4 childBg               = colors.surfaceContainerLow;
    childBg.w                    = 0.50f;
    imguiColor[ImGuiCol_ChildBg] = childBg;

    // Popup 背景
    ImVec4 popupBg               = colors.surfaceContainer;
    popupBg.w                    = 0.98f;
    imguiColor[ImGuiCol_PopupBg] = popupBg;

    // 边框
    imguiColor[ImGuiCol_Border]       = colors.outlineVariant;
    imguiColor[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    // 文本
    imguiColor[ImGuiCol_Text]         = colors.onSurface;
    imguiColor[ImGuiCol_TextDisabled] = colors.onSurfaceVariant;

    // 标题栏
    imguiColor[ImGuiCol_TitleBg]          = colors.surfaceContainerLow;
    imguiColor[ImGuiCol_TitleBgActive]    = colors.surfaceContainerLow;
    imguiColor[ImGuiCol_TitleBgCollapsed] = colors.surfaceContainerLow;

    // 滚动条
    imguiColor[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    imguiColor[ImGuiCol_ScrollbarGrab]        = colors.outlineVariant;
    imguiColor[ImGuiCol_ScrollbarGrabHovered] = colors.onSurfaceVariant;
    imguiColor[ImGuiCol_ScrollbarGrabActive]  = colors.onSurface;

    // Frame 控件（Input、Combo 等）
    imguiColor[ImGuiCol_FrameBg] = colors.surfaceContainerHighest;
    imguiColor[ImGuiCol_FrameBgHovered] =
        ApplyStateLayer(colors.surfaceContainerHighest, colors.onSurface, colors.stateLayerHover);
    imguiColor[ImGuiCol_FrameBgActive] =
        ApplyStateLayer(colors.surfaceContainerHighest, colors.onSurface, colors.stateLayerPressed);

    // 按钮
    imguiColor[ImGuiCol_Button] = colors.surfaceContainerHigh;
    imguiColor[ImGuiCol_ButtonHovered] =
        ApplyStateLayer(colors.surfaceContainerHigh, colors.onSurface, colors.stateLayerHover);
    imguiColor[ImGuiCol_ButtonActive] = colors.primary;

    // 复选框/滑块
    imguiColor[ImGuiCol_CheckMark]        = colors.primary;
    imguiColor[ImGuiCol_SliderGrab]       = colors.primary;
    imguiColor[ImGuiCol_SliderGrabActive] = colors.primary;

    // Header（CollapsingHeader、TreeNode 等）
    imguiColor[ImGuiCol_Header] = colors.surfaceContainerHigh;
    imguiColor[ImGuiCol_HeaderHovered] =
        ApplyStateLayer(colors.surfaceContainerHigh, colors.onSurface, colors.stateLayerHover);
    imguiColor[ImGuiCol_HeaderActive] =
        ApplyStateLayer(colors.surfaceContainerHigh, colors.onSurface, colors.stateLayerPressed);

    // 分隔符
    imguiColor[ImGuiCol_Separator]        = colors.outlineVariant;
    imguiColor[ImGuiCol_SeparatorHovered] = colors.outline;
    imguiColor[ImGuiCol_SeparatorActive]  = colors.primary;

    // Resize 手柄
    imguiColor[ImGuiCol_ResizeGrip]        = colors.outlineVariant;
    imguiColor[ImGuiCol_ResizeGripHovered] = colors.onSurfaceVariant;
    imguiColor[ImGuiCol_ResizeGripActive]  = colors.primary;

    // Tab
    imguiColor[ImGuiCol_Tab] = colors.surfaceContainerHigh;
    imguiColor[ImGuiCol_TabHovered] =
        ApplyStateLayer(colors.surfaceContainerHigh, colors.primary, colors.stateLayerHover);
    imguiColor[ImGuiCol_TabSelected]         = colors.secondaryContainer;
    imguiColor[ImGuiCol_TabSelectedOverline] = colors.primary;
    imguiColor[ImGuiCol_TabDimmed]           = colors.surfaceContainer;
    imguiColor[ImGuiCol_TabDimmedSelected]   = colors.surfaceContainerHigh;

    // 表格
    imguiColor[ImGuiCol_TableHeaderBg]     = colors.surfaceContainerHigh;
    imguiColor[ImGuiCol_TableBorderStrong] = colors.outline;
    imguiColor[ImGuiCol_TableBorderLight]  = colors.outlineVariant;
    imguiColor[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    imguiColor[ImGuiCol_TableRowBgAlt]     = ApplyStateLayer(colors.surface, colors.onSurface, 0.02f);

    // 其他
    imguiColor[ImGuiCol_PlotLines]             = colors.primary;
    imguiColor[ImGuiCol_PlotLinesHovered]      = colors.tertiary;
    imguiColor[ImGuiCol_PlotHistogram]         = colors.primary;
    imguiColor[ImGuiCol_PlotHistogramHovered]  = colors.tertiary;
    imguiColor[ImGuiCol_TextSelectedBg]        = ApplyStateLayer(colors.surface, colors.primary, 0.35f);
    imguiColor[ImGuiCol_DragDropTarget]        = colors.primary;
    imguiColor[ImGuiCol_NavHighlight]          = colors.primary;
    imguiColor[ImGuiCol_NavWindowingHighlight] = colors.primary;
    imguiColor[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.20f);
    imguiColor[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.35f);
}
#endif // MD3_BACKEND_DILIGENT

void AddImageRounded(ImDrawList* dl, void* tex_id, const ImVec2& p_min, const ImVec2& p_max, const ImVec2& uv_min,
                     const ImVec2& uv_max, unsigned int col, float rounding, int flags) {
    if ((col & IM_COL32_A_MASK) == 0 || rounding < 0.5f) {
        // 无透明度或无圆角，直接使用普通 AddImage
        dl->AddImage((ImTextureID)tex_id, p_min, p_max, uv_min, uv_max, col);
        return;
    }

    // 创建圆角矩形路径
    dl->PathRect(p_min, p_max, rounding, flags);

    // 获取路径点数量
    int path_count = dl->_Path.Size;
    if (path_count < 3) {
        dl->PathClear();
        return;
    }

    // 计算尺寸用于 UV 映射
    float inv_w = 1.0f / (p_max.x - p_min.x);
    float inv_h = 1.0f / (p_max.y - p_min.y);
    float uv_w  = uv_max.x - uv_min.x;
    float uv_h  = uv_max.y - uv_min.y;

    // 切换到指定纹理
    dl->PushTextureID((ImTextureID)tex_id);

    // 预留顶点和索引空间（三角形扇形）
    int idx_count = (path_count - 2) * 3;
    dl->PrimReserve(idx_count, path_count);

    // 获取当前顶点索引基址
    ImDrawIdx idx_base = (ImDrawIdx)dl->_VtxCurrentIdx;

    // 添加顶点（带 UV 计算）
    for (int i = 0; i < path_count; i++) {
        ImVec2 p = dl->_Path[i];
        // 计算 UV：线性插值
        float u = uv_min.x + (p.x - p_min.x) * inv_w * uv_w;
        float v = uv_min.y + (p.y - p_min.y) * inv_h * uv_h;
        dl->PrimWriteVtx(p, ImVec2(u, v), col);
    }

    // 添加索引（三角形扇形）
    for (int i = 2; i < path_count; i++) {
        dl->PrimWriteIdx(idx_base);
        dl->PrimWriteIdx((ImDrawIdx)(idx_base + i - 1));
        dl->PrimWriteIdx((ImDrawIdx)(idx_base + i));
    }

    dl->PopTextureID();
    dl->PathClear();
}

} // namespace MD3

//=============================================================================
// ImGui 集成钩子实现
//=============================================================================

#ifdef IMGUI_MD3_ENABLED

extern "C" void MD3_OnNewFrame(float dt) {
    MD3::BeginFrame(dt);
}

extern "C" void MD3_TriggerRipple(unsigned int id, float mouseX, float mouseY, float bbMinX, float bbMinY, float bbMaxX,
                                  float bbMaxY) {
    float cornerRadius = ImGui::GetStyle().FrameRounding;
    MD3::TriggerRipple(id, mouseX, mouseY, bbMinX, bbMinY, bbMaxX - bbMinX, bbMaxY - bbMinY, cornerRadius);
}

extern "C" bool MD3_Checkbox(const char* label, bool* v) {
    return MD3::Toggle(label, v);
}

#endif
