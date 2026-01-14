// MD3Context.cpp - MD3 上下文管理（Diligent 版本）
// 包含 Ripple 系统、Stencil 圆角裁剪的 Diligent 实现

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <iostream>

#include "../ImGuiDiligent.h"
#include "Buffer.h"
#include "DeviceContext.h"
#include "MD3.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"

namespace MD3 {

// 全局上下文
static MD3Context g_Context;

MD3Context& GetContext() {
    return g_Context;
}

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

//=============================================================================
// Diligent 资源创建
//=============================================================================

static bool CreateRipplePSO(Diligent::IRenderDevice* device) {
    using namespace Diligent;

    auto& ctx = GetContext();

    // 创建 Vertex Shader
    ShaderCreateInfo shaderCI;
    shaderCI.SourceLanguage  = SHADER_SOURCE_LANGUAGE_HLSL;
    shaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
    shaderCI.Desc.Name       = "MD3 Ripple VS";
    shaderCI.Source          = RippleVS;
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
    shaderCI.Source          = RipplePS;

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
    psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_D32_FLOAT;

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

    RefCntAutoPtr<IPipelineState> pPSO;
    device->CreateGraphicsPipelineState(psoCI, &pPSO);
    if (!pPSO) {
        std::cerr << "[MD3] Failed to create Ripple PSO" << std::endl;
        return false;
    }

    ctx.diligent.ripplePSO = pPSO.Detach();

    // 创建 SRB
    RefCntAutoPtr<IShaderResourceBinding> pSRB;
    ctx.diligent.ripplePSO->CreateShaderResourceBinding(&pSRB, true);
    ctx.diligent.rippleSRB = pSRB.Detach();

    // 创建 Constants buffer
    BufferDesc cbDesc;
    cbDesc.Name           = "MD3 Ripple Constants";
    cbDesc.Size           = 64; // 足够存储所有 uniform
    cbDesc.Usage          = USAGE_DYNAMIC;
    cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

    RefCntAutoPtr<IBuffer> pCB;
    device->CreateBuffer(cbDesc, nullptr, &pCB);
    ctx.diligent.rippleConstants = pCB.Detach();

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

void Init(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, float dpiScale) {
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
    ImGui::GetStyle().CircleTessellationMaxError = 0.1f; // 默认是 0.3f，更小 = 更多段数

    // 保存 Diligent 设备引用
    ctx.diligent.device  = device;
    ctx.diligent.context = context;

    // 创建 Ripple 渲染资源
    if (device && context) {
        CreateRipplePSO(device);
        ctx.diligent.initialized = true;
    }

    ctx.initialized = true;
    std::cout << "[MD3] Material Design 3 UI system initialized (Diligent)" << std::endl;
}

void Shutdown() {
    auto& ctx = GetContext();
    if (!ctx.initialized) {
        return;
    }

    // 释放 Diligent 资源
    if (ctx.diligent.ripplePSO) {
        ctx.diligent.ripplePSO->Release();
        ctx.diligent.ripplePSO = nullptr;
    }
    if (ctx.diligent.rippleSRB) {
        ctx.diligent.rippleSRB->Release();
        ctx.diligent.rippleSRB = nullptr;
    }
    if (ctx.diligent.rippleConstants) {
        ctx.diligent.rippleConstants->Release();
        ctx.diligent.rippleConstants = nullptr;
    }
    if (ctx.diligent.rippleVB) {
        ctx.diligent.rippleVB->Release();
        ctx.diligent.rippleVB = nullptr;
    }
    ctx.diligent.device      = nullptr;
    ctx.diligent.context     = nullptr;
    ctx.diligent.initialized = false;

    // 清理 Ripple 状态
    ctx.ripples.clear();

    // 清理动画状态缓存
    ctx.toggleStates.clear();
    ctx.buttonStates.clear();
    ctx.sliderStates.clear();
    ctx.cardStates.clear();
    ctx.comboStates.clear();
    ctx.selectableStates.clear();
    ctx.collapsingHeaderStates.clear();
    ctx.windowStates.clear();
    ctx.scrollbarStates.clear();
    ctx.resizeStates.clear();
    ctx.smoothScrollStates.clear();

    ctx.initialized = false;
    std::cout << "[MD3] Material Design 3 UI system shutdown" << std::endl;
}

void BeginFrame(float dt) {
    auto& ctx = GetContext();
    if (!ctx.initialized) {
        return;
    }

    ctx.deltaTime = dt;
    ctx.currentTime += dt;
    ctx.frameIndex++;

    // 更新所有 Ripple 动画
    auto&       ripples = ctx.ripples;
    const auto& config  = ctx.rippleConfig;

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

    // 更新所有活跃的动画状态
    for (auto& [id, state] : ctx.toggleStates) {
        state.knobPosition.Update(dt);
        state.trackFill.Update(dt);
        state.knobScale.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : ctx.buttonStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
        state.pressState.Update(dt);
    }

    for (auto& [id, state] : ctx.sliderStates) {
        state.thumbScale.Update(dt);
        state.activeTrack.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : ctx.cardStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : ctx.comboStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    for (auto& [id, state] : ctx.selectableStates) {
        // 如果上一帧没有看到这个 selectable，重置悬停状态
        int expectedFrameSeen = ctx.frameIndex - 1;
        if (state.lastFrameSeen != expectedFrameSeen) {
            state.hoverState.target = 0.0f;
        }
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : ctx.collapsingHeaderStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    for (auto& [id, state] : ctx.windowStates) {
        state.closeButtonHover.Update(dt);
        state.closeButtonPress.Update(dt);
        state.openProgress.Update(dt);
        state.scale.Update(dt);
        state.offsetY.Update(dt);
        state.alpha.Update(dt);
    }

    for (auto& [id, state] : ctx.scrollbarStates) {
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

    for (auto& [id, state] : ctx.resizeStates) {
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : ctx.smoothScrollStates) {
        state.scrollY.Update(dt);
    }
}

void EndFrame() {
    // Diligent 版本不需要渲染 Ripple shader
    // Ripple 效果可以用 ImGui DrawList 实现（如果需要）
}

void SetDarkMode(bool dark) {
    auto& ctx = GetContext();
    if (ctx.isDarkMode == dark) {
        return;
    }

    ctx.isDarkMode = dark;
    ctx.colors     = dark ? GetDarkColorScheme() : GetLightColorScheme();
}

bool IsDarkMode() {
    return GetContext().isDarkMode;
}

void SetScreenSize(float width, float height) {
    auto& ctx        = GetContext();
    ctx.screenWidth  = width;
    ctx.screenHeight = height;
}

void SetDpiScale(float scale) {
    GetContext().dpiScale = scale;
}

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

//=============================================================================
// 工具函数
//=============================================================================

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
    return IM_COL32((int)(color.x * 255.0f + 0.5f), (int)(color.y * 255.0f + 0.5f), (int)(color.z * 255.0f + 0.5f),
                    (int)(color.w * 255.0f + 0.5f));
}

ImVec4 HexToColor(unsigned int hex, float alpha) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

//=============================================================================
// Ripple API 实现
//=============================================================================

void TriggerRipple(ImGuiID id, float centerX, float centerY, float boundsX, float boundsY, float boundsW, float boundsH,
                   float cornerRadius) {
    auto& ctx = GetContext();

    // 计算最大半径（覆盖整个控件对角线）
    float dx1 = centerX - boundsX;
    float dx2 = (boundsX + boundsW) - centerX;
    float dy1 = centerY - boundsY;
    float dy2 = (boundsY + boundsH) - centerY;

    float maxDx     = std::max(dx1, dx2);
    float maxDy     = std::max(dy1, dy2);
    float maxRadius = std::sqrt(maxDx * maxDx + maxDy * maxDy);

    // 获取 Ripple 颜色
    const auto& colors      = ctx.colors;
    ImVec4      rippleColor = ctx.isDarkMode ? colors.onSurface : colors.primary;

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

    ctx.ripples.push_back(state);
}

void TriggerRippleForCurrentItem(ImGuiID id, float cornerRadius) {
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    ImVec2 itemMin  = ImGui::GetItemRectMin();
    ImVec2 itemMax  = ImGui::GetItemRectMax();

    TriggerRipple(id, mousePos.x, mousePos.y, itemMin.x, itemMin.y, itemMax.x - itemMin.x, itemMax.y - itemMin.y,
                  cornerRadius);
}

void DrawRipples() {
    auto& ctx = GetContext();
    if (ctx.ripples.empty()) {
        return;
    }

    // 获取当前窗口
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) {
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 绘制属于当前窗口的所有 Ripple（使用 ImDrawList 实现）
    for (const auto& r : ctx.ripples) {
        if (r.alpha <= 0.001f) {
            continue;
        }
        if (r.windowId != window->ID) {
            continue; // 只绘制当前窗口的 ripple
        }

        // 计算滚动偏移量
        float scrollDeltaX = window->Scroll.x - r.initialScrollX;
        float scrollDeltaY = window->Scroll.y - r.initialScrollY;

        // 计算当前控件位置（补偿滚动）
        float currentBoundsX = r.initialBoundsX - scrollDeltaX;
        float currentBoundsY = r.initialBoundsY - scrollDeltaY;

        // 计算 ripple 中心的屏幕位置
        float centerX = currentBoundsX + r.relCenterX;
        float centerY = currentBoundsY + r.relCenterY;

        // 保存裁剪区域
        ImVec2 clipMin(currentBoundsX, currentBoundsY);
        ImVec2 clipMax(currentBoundsX + r.boundsW, currentBoundsY + r.boundsH);
        dl->PushClipRect(clipMin, clipMax, true);

        // 使用 ImDrawList 渲染圆形 Ripple
        ImVec4 rippleColor(r.colorR, r.colorG, r.colorB, r.alpha);
        ImU32  col = ColorToU32(rippleColor);
        dl->AddCircleFilled(ImVec2(centerX, centerY), r.radius, col, 64);

        dl->PopClipRect();
    }
}

void DrawRipplesDiligent() {
    auto& ctx = GetContext();
    if (ctx.ripples.empty() || !ctx.diligent.initialized) {
        // 如果 Diligent 未初始化，使用 ImDrawList 回退
        DrawRipples();
        return;
    }

    // TODO: 使用 Diligent PSO 渲染 Ripple（带精确圆角裁剪）
    // 目前先使用 ImDrawList 实现
    DrawRipples();
}

//=============================================================================
// 圆角裁剪（使用 Diligent Stencil 实现）
//=============================================================================

// Stencil 裁剪栈信息
struct RoundedClipInfo {
    ImVec4 rect;       // 裁剪矩形 (x1, y1, x2, y2)
    float  rounding;   // 圆角半径
    int    stencilRef; // Stencil 参考值
};

static std::vector<RoundedClipInfo> s_roundedClipStack;
static int                          s_stencilRefCounter = 0;

// Stencil 写入回调：绘制圆角矩形到 Stencil 缓冲
static void RoundedClipBeginCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui == nullptr) {
        return;
    }

    // 从 UserCallbackData 获取裁剪信息
    auto* clipInfo = static_cast<RoundedClipInfo*>(cmd->UserCallbackData);
    if (clipInfo == nullptr) {
        return;
    }

    // 设置 Stencil 写入模式
    imgui->SetStencilMode(StencilMode::WriteIncr, clipInfo->stencilRef);
}

// Stencil 测试回调：启用 Stencil 测试
static void RoundedClipTestCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui == nullptr) {
        return;
    }

    auto* clipInfo = static_cast<RoundedClipInfo*>(cmd->UserCallbackData);
    if (clipInfo == nullptr) {
        return;
    }

    // 设置 Stencil 测试模式
    imgui->SetStencilMode(StencilMode::TestEqual, clipInfo->stencilRef);
}

// 恢复正常渲染回调
static void RoundedClipEndCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    using namespace ParticleSaturn::UI;

    auto* imgui = GetImGuiDiligentInstance();
    if (imgui == nullptr) {
        return;
    }

    auto* clipInfo = static_cast<RoundedClipInfo*>(cmd->UserCallbackData);
    int   newRef   = (clipInfo != nullptr && !s_roundedClipStack.empty()) ? s_roundedClipStack.back().stencilRef : 0;

    // 恢复到上一层的 Stencil 测试（或禁用）
    if (newRef > 0) {
        imgui->SetStencilMode(StencilMode::TestEqual, newRef);
    } else {
        imgui->SetStencilMode(StencilMode::Disabled, 0);
    }
}

void PushRoundedClipRect(const ImVec2& clip_min, const ImVec2& clip_max, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    // 增加 Stencil 参考值
    s_stencilRefCounter++;

    // 保存裁剪信息
    RoundedClipInfo clipInfo;
    clipInfo.rect       = ImVec4(clip_min.x, clip_min.y, clip_max.x, clip_max.y);
    clipInfo.rounding   = rounding;
    clipInfo.stencilRef = s_stencilRefCounter;
    s_roundedClipStack.push_back(clipInfo);

    // 设置矩形裁剪（作为基础裁剪）
    dl->PushClipRect(clip_min, clip_max, true);

    // 添加回调：开始 Stencil 写入
    dl->AddCallback(RoundedClipBeginCallback, &s_roundedClipStack.back());

    // 绘制圆角矩形到 Stencil（使用白色，实际颜色不重要因为不写入颜色缓冲）
    dl->AddRectFilled(clip_min, clip_max, IM_COL32_WHITE, rounding);

    // 添加回调：切换到 Stencil 测试模式
    dl->AddCallback(RoundedClipTestCallback, &s_roundedClipStack.back());
}

void PopRoundedClipRect() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    if (s_roundedClipStack.empty()) {
        return;
    }

    // 添加回调：恢复 Stencil 状态
    dl->AddCallback(RoundedClipEndCallback, &s_roundedClipStack.back());

    // 弹出裁剪信息
    s_roundedClipStack.pop_back();

    // 更新 Stencil 参考值计数
    s_stencilRefCounter = s_roundedClipStack.empty() ? 0 : s_roundedClipStack.back().stencilRef;

    dl->PopClipRect();
}

//=============================================================================
// AddImageRounded 实现
//=============================================================================

void AddImageRounded(ImDrawList* dl, ImTextureID tex_id, const ImVec2& p_min, const ImVec2& p_max, const ImVec2& uv_min,
                     const ImVec2& uv_max, unsigned int col, float rounding, int flags) {
    if ((col & IM_COL32_A_MASK) == 0 || rounding < 0.5f) {
        // 无透明度或无圆角，直接使用普通 AddImage
        dl->AddImage(tex_id, p_min, p_max, uv_min, uv_max, col);
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
    dl->PushTextureID(tex_id);

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
