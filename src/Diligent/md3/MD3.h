#pragma once

// MD3.h - Material Design 3 完整 UI 系统（Diligent Engine 版本）
// 包含弹簧动画、Ripple 效果、MD3 色彩系统和控件
// 使用 Diligent Engine 实现 Ripple shader 和 Stencil 圆角裁剪

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

// 需要根据渲染后端选择 Ripple shader 语言（HLSL/GLSL）
#include "../RenderBackend.h"

// Diligent 前向声明
namespace Diligent {
class IRenderDevice;
class IDeviceContext;
class IPipelineState;
class IShaderResourceBinding;
class IBuffer;
} // namespace Diligent

// ImGui 前向声明
struct ImVec2;
struct ImVec4;
struct ImRect;
struct ImDrawList;
typedef unsigned int ImGuiID;

namespace MD3 {

//=============================================================================
// 弹簧动画系统
//=============================================================================

// 弹簧动画器 - 使用阻尼谐振器模型
struct SpringAnimator {
    float value     = 0.0f;
    float velocity  = 0.0f;
    float target    = 0.0f;
    float stiffness = 300.0f;
    float damping   = 22.0f;

    void Update(float dt) {
        if (!std::isfinite(dt) || dt <= 0.0f) {
            return;
        }

        if (!std::isfinite(value) || !std::isfinite(target) || !std::isfinite(velocity) || !std::isfinite(stiffness) ||
            !std::isfinite(damping)) {
            if (!std::isfinite(target)) {
                target = 0.0f;
            }
            value    = target;
            velocity = 0.0f;
            return;
        }

        dt = std::clamp(dt, 0.0f, 0.5f);

        constexpr float kMaxStep = 1.0f / 120.0f;
        int             steps    = (dt > kMaxStep) ? (int)(dt / kMaxStep) + 1 : 1;
        steps                    = std::min(steps, 64);
        float stepDt             = dt / (float)steps;

        for (int i = 0; i < steps; i++) {
            float force  = stiffness * (target - value);
            float damper = -damping * velocity;
            velocity += (force + damper) * stepDt;
            value += velocity * stepDt;

            if (!std::isfinite(value) || !std::isfinite(velocity)) {
                value    = target;
                velocity = 0.0f;
                break;
            }
        }
    }

    bool IsSettled(float threshold = 0.001f) const {
        return std::abs(target - value) < threshold && std::abs(velocity) < threshold;
    }

    void SnapToTarget() {
        value    = target;
        velocity = 0.0f;
    }

    void SetTarget(float t) { target = t; }

    SpringAnimator(float initial = 0.0f, float stiff = 300.0f, float damp = 22.0f)
        : value(initial), target(initial), stiffness(stiff), damping(damp) {}
};

// 2D 弹簧动画器
struct SpringAnimator2D {
    SpringAnimator x, y;

    void Update(float dt) {
        x.Update(dt);
        y.Update(dt);
    }

    bool IsSettled(float threshold = 0.001f) const { return x.IsSettled(threshold) && y.IsSettled(threshold); }

    void SetTarget(float tx, float ty) {
        x.target = tx;
        y.target = ty;
    }
};

//=============================================================================
// MD3 色彩系统
//=============================================================================

struct MD3ColorScheme {
    // Primary 系列
    ImVec4 primary;
    ImVec4 onPrimary;
    ImVec4 primaryContainer;
    ImVec4 onPrimaryContainer;

    // Secondary 系列
    ImVec4 secondary;
    ImVec4 onSecondary;
    ImVec4 secondaryContainer;
    ImVec4 onSecondaryContainer;

    // Tertiary 系列
    ImVec4 tertiary;
    ImVec4 onTertiary;
    ImVec4 tertiaryContainer;
    ImVec4 onTertiaryContainer;

    // Error 系列
    ImVec4 error;
    ImVec4 onError;
    ImVec4 errorContainer;
    ImVec4 onErrorContainer;

    // Surface 系列
    ImVec4 surface;
    ImVec4 surfaceDim;
    ImVec4 surfaceBright;
    ImVec4 surfaceVariant;
    ImVec4 onSurface;
    ImVec4 onSurfaceVariant;

    // Surface Container 层次
    ImVec4 surfaceContainerLowest;
    ImVec4 surfaceContainerLow;
    ImVec4 surfaceContainer;
    ImVec4 surfaceContainerHigh;
    ImVec4 surfaceContainerHighest;

    // Outline 系列
    ImVec4 outline;
    ImVec4 outlineVariant;

    // 其他
    ImVec4 inverseSurface;
    ImVec4 inverseOnSurface;
    ImVec4 inversePrimary;
    ImVec4 shadow;
    ImVec4 scrim;

    // 状态层透明度
    float stateLayerHover   = 0.08f;
    float stateLayerFocused = 0.12f;
    float stateLayerPressed = 0.12f;
    float stateLayerDragged = 0.16f;
};

MD3ColorScheme GetLightColorScheme();
MD3ColorScheme GetDarkColorScheme();

//=============================================================================
// 控件动画状态
//=============================================================================

struct ToggleAnimState {
    SpringAnimator knobPosition;
    SpringAnimator trackFill;
    SpringAnimator knobScale;
    SpringAnimator hoverState;

    ToggleAnimState()
        : knobPosition(0.0f, 300.0f, 22.0f),
          trackFill(0.0f, 300.0f, 22.0f),
          knobScale(1.0f, 400.0f, 25.0f),
          hoverState(0.0f, 500.0f, 30.0f) {}
};

struct ButtonAnimState {
    SpringAnimator elevation;
    SpringAnimator hoverState;
    SpringAnimator pressState;

    ButtonAnimState()
        : elevation(0.0f, 400.0f, 28.0f), hoverState(0.0f, 500.0f, 30.0f), pressState(0.0f, 600.0f, 35.0f) {}
};

struct SliderAnimState {
    SpringAnimator thumbScale;
    SpringAnimator activeTrack;
    SpringAnimator hoverState;

    SliderAnimState()
        : thumbScale(1.0f, 400.0f, 25.0f), activeTrack(0.0f, 800.0f, 45.0f), hoverState(0.0f, 500.0f, 30.0f) {}
};

struct CardAnimState {
    SpringAnimator elevation;
    SpringAnimator hoverState;

    CardAnimState() : elevation(1.0f, 300.0f, 25.0f), hoverState(0.0f, 400.0f, 28.0f) {}
};

struct ComboAnimState {
    SpringAnimator hoverState;
    SpringAnimator openState;
    SpringAnimator arrowRotation;
    float          lastContentHeight = 0.0f;

    ComboAnimState()
        : hoverState(0.0f, 500.0f, 30.0f), openState(0.0f, 800.0f, 40.0f), arrowRotation(0.0f, 800.0f, 40.0f) {}
};

struct SelectableAnimState {
    SpringAnimator hoverState;
    int            lastFrameSeen = -1;

    SelectableAnimState() : hoverState(0.0f, 500.0f, 30.0f) {}
};

struct CollapsingHeaderAnimState {
    SpringAnimator hoverState;
    SpringAnimator openState;
    SpringAnimator arrowRotation;
    float          lastContentHeight;

    CollapsingHeaderAnimState()
        : hoverState(0.0f, 500.0f, 30.0f),
          openState(0.0f, 350.0f, 38.0f),     // damping 38 ≥ 2*sqrt(350)≈37.4 临界阻尼，消除振荡
          arrowRotation(0.0f, 350.0f, 26.0f), // 箭头保持弹性动画效果
          lastContentHeight(0.0f) {}
};

struct WindowAnimState {
    SpringAnimator closeButtonHover;
    SpringAnimator closeButtonPress;
    SpringAnimator openProgress;
    SpringAnimator scale;
    SpringAnimator offsetY;
    SpringAnimator alpha;

    enum class LifecycleState {
        Closed,
        Opening,
        Open,
        Closing
    };
    LifecycleState lifecycleState = LifecycleState::Closed;
    bool           wantClose      = false;
    bool           firstFrame     = true;

    WindowAnimState()
        : closeButtonHover(0.0f, 500.0f, 30.0f),
          closeButtonPress(0.0f, 600.0f, 35.0f),
          openProgress(0.0f, 450.0f, 30.0f),
          scale(0.85f, 450.0f, 30.0f),
          offsetY(12.0f, 450.0f, 30.0f),
          alpha(0.0f, 500.0f, 35.0f) {}
};

struct ScrollbarAnimState {
    SpringAnimator hoverState;
    SpringAnimator dragState;
    SpringAnimator visibility;
    float          lastScrollY = 0.0f;
    float          hideTimer   = 0.0f;

    ScrollbarAnimState()
        : hoverState(0.0f, 500.0f, 30.0f), dragState(0.0f, 600.0f, 35.0f), visibility(0.0f, 400.0f, 28.0f) {}
};

struct ResizeAnimState {
    SpringAnimator hoverState;
    bool           isDragging      = false;
    float          dragStartMouseX = 0.0f;
    float          dragStartMouseY = 0.0f;
    float          dragStartSizeW  = 0.0f;
    float          dragStartSizeH  = 0.0f;

    ResizeAnimState() : hoverState(0.0f, 500.0f, 30.0f) {}
};

struct SmoothScrollState {
    SpringAnimator scrollY;
    float          lastAppliedScrollY = 0.0f;
    bool           initialized        = false;
    int            lastFrameProcessed = -1;
    float          lastWheelTime      = -1.0f;
    int            lastWheelDir       = 0;
    int            wheelStreak        = 0;
};

//=============================================================================
// Ripple 状态管理
//=============================================================================

// 单个 Ripple 的状态
struct RippleState {
    ImGuiID widgetId = 0;
    // 所有坐标都存储为相对于控件的偏移量
    float relCenterX = 0.0f; // 点击点相对于 bounds 左上角的 X 偏移
    float relCenterY = 0.0f; // 点击点相对于 bounds 左上角的 Y 偏移
    float radius     = 0.0f;
    float maxRadius  = 0.0f;
    float alpha      = 0.0f;
    float time       = 0.0f;
    // 控件尺寸（不变）
    float boundsW      = 0.0f;
    float boundsH      = 0.0f;
    float cornerRadius = 0.0f;
    // 颜色
    float colorR = 0.0f;
    float colorG = 0.0f;
    float colorB = 0.0f;
    float colorA = 0.0f;
    // 所属窗口信息（用于滚动补偿）
    ImGuiID windowId          = 0;
    float   initialWindowPosX = 0.0f;
    float   initialWindowPosY = 0.0f;
    float   initialScrollX    = 0.0f;
    float   initialScrollY    = 0.0f;
    float   initialBoundsX    = 0.0f;
    float   initialBoundsY    = 0.0f;
    bool    active            = false;
    bool    fadeOut           = false;
};

// Ripple 系统配置
struct RippleConfig {
    float expandDuration = 0.225f;
    float fadeDuration   = 0.150f;
    float maxAlpha       = 0.12f;
};

//=============================================================================
// Diligent 渲染资源
//=============================================================================

// Diligent 渲染上下文（用于 Ripple 和 Stencil）
struct DiligentRenderContext {
    Diligent::IRenderDevice*          device          = nullptr;
    Diligent::IDeviceContext*         context         = nullptr;
    Diligent::IPipelineState*         ripplePSO       = nullptr;
    Diligent::IShaderResourceBinding* rippleSRB       = nullptr;
    Diligent::IBuffer*                rippleConstants = nullptr;
    Diligent::IBuffer*                rippleVB        = nullptr;

    // Stencil PSO for rounded clip
    Diligent::IPipelineState* stencilWritePSO = nullptr;
    Diligent::IPipelineState* stencilTestPSO  = nullptr;

    bool initialized = false;
};

//=============================================================================
// MD3 上下文
//=============================================================================

struct MD3Context {
    bool  initialized = false;
    bool  isDarkMode  = true;
    float dpiScale    = 1.0f;
    float deltaTime   = 0.0f;
    float currentTime = 0.0f;
    int   frameIndex  = 0;

    MD3ColorScheme colors;

    // Ripple 系统
    RippleConfig             rippleConfig;
    std::vector<RippleState> ripples;

    // Diligent 渲染资源
    DiligentRenderContext diligent;

    // 控件动画状态缓存
    std::unordered_map<ImGuiID, ToggleAnimState>           toggleStates;
    std::unordered_map<ImGuiID, ButtonAnimState>           buttonStates;
    std::unordered_map<ImGuiID, SliderAnimState>           sliderStates;
    std::unordered_map<ImGuiID, CardAnimState>             cardStates;
    std::unordered_map<ImGuiID, ComboAnimState>            comboStates;
    std::unordered_map<ImGuiID, SelectableAnimState>       selectableStates;
    std::unordered_map<ImGuiID, CollapsingHeaderAnimState> collapsingHeaderStates;
    std::unordered_map<ImGuiID, WindowAnimState>           windowStates;
    std::unordered_map<ImGuiID, ScrollbarAnimState>        scrollbarStates;
    std::unordered_map<ImGuiID, ResizeAnimState>           resizeStates;
    std::unordered_map<ImGuiID, SmoothScrollState>         smoothScrollStates;

    float screenWidth  = 1920.0f;
    float screenHeight = 1080.0f;

    // 模糊纹理（用于窗口背景玻璃效果）
    void* blurTextureID  = nullptr; // ImTextureID (ITextureView*) - 1/6 分辨率强模糊
    void* blurTextureID2 = nullptr; // ImTextureID (ITextureView*) - 1/12 分辨率弱模糊（折叠区域）
    void* noiseTextureID = nullptr; // ImTextureID (ITextureView*) - 全分辨率噪点（用于防 banding + 质感）
    bool  blurEnabled    = false;
};

MD3Context& GetContext();

//=============================================================================
// 公共 API
//=============================================================================

// 初始化 MD3 系统（Diligent 版本）
void Init(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, ParticleSaturn::Render::Backend backend,
          float dpiScale = 1.0f);

// 关闭 MD3 系统
void Shutdown();

// 每帧开始时调用
void BeginFrame(float dt);

// 每帧结束时调用（在 ImGui::Render 之前）
void EndFrame();

// 设置深色/浅色模式
void SetDarkMode(bool dark);

// 获取当前模式
bool IsDarkMode();

// 设置屏幕尺寸
void SetScreenSize(float width, float height);

// 设置 DPI 缩放
void SetDpiScale(float scale);

// 设置模糊纹理（用于窗口背景玻璃效果）
void SetBlurTexture(void* textureID, bool enabled);

// 设置次级模糊纹理（用于折叠区域 Acrylic 效果，1/12 分辨率弱模糊）
void SetBlurTexture2(void* textureID);

// 设置噪点纹理（全分辨率，避免依赖 wrap sampler）
void SetNoiseTexture(void* textureID);

// 应用 MD3 主题到 ImGui 样式（窗口、表格、滚动条等）
void ApplyImGuiStyle();

//=============================================================================
// MD3 控件
//=============================================================================

bool  Toggle(const char* label, bool* v);
bool  FilledButton(const char* label, ImVec2 size = {0, 0});
bool  TonalButton(const char* label, ImVec2 size = {0, 0});
bool  OutlinedButton(const char* label, ImVec2 size = {0, 0});
bool  TextButton(const char* label);
bool  Button(const char* label, ImVec2 size = {0, 0});
bool  Slider(const char* label, float* v, float min, float max, const char* format = "%.1f");
bool  BeginCard(const char* id, ImVec2 size = {0, 0}, int elevation = 1);
void  EndCard();
bool  BeginCombo(const char* label, const char* preview_value);
void  EndCombo();
bool  Selectable(const char* label, bool selected);
bool  Combo(const char* label, int* current_item, const char* const items[], int items_count);
bool  BeginCollapsingHeader(const char* label, bool default_open = false);
void  EndCollapsingHeader();
bool  BeginWindow(const char* title, bool* p_open = nullptr, int flags = 0);
void  EndWindow();
float WindowTitleBarSpace();
void  WindowTitleBar(const char* title, bool* p_open = nullptr);
void  WindowScrollbar(float titleBarHeight = 0.0f);
void  WindowResize(float minWidth = 200.0f, float minHeight = 100.0f);
void  HandleSmoothScroll(float scrollSpeed = 90.0f);

// MenuItem（用于右键菜单等弹出菜单）
bool MenuItem(const char* label, bool enabled = true, float height_override = 0.0f);

// 圆角裁剪（使用 stencil）
void PushRoundedClipRect(const ImVec2& clip_min, const ImVec2& clip_max, float rounding);
void PopRoundedClipRect();

//=============================================================================
// Ripple 系统 API
//=============================================================================

// 触发 Ripple 效果
void TriggerRipple(ImGuiID id, float centerX, float centerY, float boundsX, float boundsY, float boundsW, float boundsH,
                   float cornerRadius = 0.0f);

// 为当前控件触发 Ripple（使用 ImGui 上下文）
void TriggerRippleForCurrentItem(ImGuiID id, float cornerRadius = 0.0f);

// 绘制所有活跃的 Ripple（在控件绘制后调用）
// 使用 ImDrawList 渲染，自动跟随滚动位置
void DrawRipples();

// 使用 Diligent 渲染 Ripple（可选，提供更精确的圆角裁剪）
void DrawRipplesDiligent();

//=============================================================================
// 工具函数
//=============================================================================

ImVec4       BlendColors(const ImVec4& base, const ImVec4& overlay, float alpha);
ImVec4       ApplyStateLayer(const ImVec4& base, const ImVec4& stateColor, float stateAlpha);
unsigned int ColorToU32(const ImVec4& color);
ImVec4       HexToColor(unsigned int hex, float alpha = 1.0f);

// 绘制带圆角的图片（解决模糊背景黑边问题）
// 注意：Diligent 版本使用 ImTextureID（可以是 ITextureView* 指针）
void AddImageRounded(ImDrawList* dl, ImTextureID tex_id, const ImVec2& p_min, const ImVec2& p_max, const ImVec2& uv_min,
                     const ImVec2& uv_max, unsigned int col, float rounding, int flags = 0);

} // namespace MD3

//=============================================================================
// ImGui 集成钩子（供 ImGui patch 使用）
//=============================================================================

#ifdef IMGUI_MD3_ENABLED

// ImGui NewFrame 钩子
extern "C" void MD3_OnNewFrame(float dt);

// ImGui ButtonBehavior 钩子
extern "C" void MD3_TriggerRipple(unsigned int id, float mouseX, float mouseY, float bbMinX, float bbMinY, float bbMaxX,
                                  float bbMaxY);

// ImGui Checkbox 替换钩子
extern "C" bool MD3_Checkbox(const char* label, bool* v);

#endif
