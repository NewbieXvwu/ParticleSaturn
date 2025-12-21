#pragma once

// MD3.h - Material Design 3 完整 UI 系统
// 包含弹簧动画、Ripple 效果、MD3 色彩系统和控件

#include <cmath>
#include <unordered_map>
#include <vector>

// 前向声明
struct ImVec2;
struct ImVec4;
struct ImRect;
typedef unsigned int ImGuiID;
typedef unsigned int GLuint;

namespace MD3 {

//=============================================================================
// 弹簧动画系统
//=============================================================================

// 弹簧动画器 - 使用阻尼谐振器模型
// 默认参数产生略欠阻尼效果（轻微回弹）
struct SpringAnimator {
    float value = 0.0f;
    float velocity = 0.0f;
    float target = 0.0f;
    float stiffness = 300.0f;  // 刚度
    float damping = 22.0f;     // 阻尼

    // 更新弹簧动画
    void Update(float dt) {
        float force = stiffness * (target - value);
        float damper = -damping * velocity;
        velocity += (force + damper) * dt;
        value += velocity * dt;
    }

    // 检查是否已稳定
    bool IsSettled(float threshold = 0.001f) const {
        return std::abs(target - value) < threshold && std::abs(velocity) < threshold;
    }

    // 立即跳转到目标值
    void SnapToTarget() {
        value = target;
        velocity = 0.0f;
    }

    // 设置目标值
    void SetTarget(float t) {
        target = t;
    }

    // 带自定义参数的构造
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

    bool IsSettled(float threshold = 0.001f) const {
        return x.IsSettled(threshold) && y.IsSettled(threshold);
    }

    void SetTarget(float tx, float ty) {
        x.target = tx;
        y.target = ty;
    }
};

//=============================================================================
// MD3 色彩系统
//=============================================================================

// MD3 完整色彩方案
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
    float stateLayerHover = 0.08f;
    float stateLayerFocused = 0.12f;
    float stateLayerPressed = 0.12f;
    float stateLayerDragged = 0.16f;
};

// 获取 Light 模式色彩方案
MD3ColorScheme GetLightColorScheme();

// 获取 Dark 模式色彩方案
MD3ColorScheme GetDarkColorScheme();

//=============================================================================
// Ripple 状态管理
//=============================================================================

// 单个 Ripple 的状态
struct RippleState {
    ImGuiID widgetId = 0;
    // 所有坐标都存储为相对于控件的偏移量
    float relCenterX = 0.0f;      // 点击点相对于 bounds 左上角的 X 偏移
    float relCenterY = 0.0f;      // 点击点相对于 bounds 左上角的 Y 偏移
    float radius = 0.0f;
    float maxRadius = 0.0f;
    float alpha = 0.0f;
    float time = 0.0f;
    // 控件尺寸（不变）
    float boundsW = 0.0f;
    float boundsH = 0.0f;
    float cornerRadius = 0.0f;
    // 颜色
    float colorR = 0.0f;
    float colorG = 0.0f;
    float colorB = 0.0f;
    float colorA = 0.0f;
    // 所属窗口信息（用于滚动补偿）
    ImGuiID windowId = 0;
    float initialWindowPosX = 0.0f;   // 创建时窗口屏幕位置
    float initialWindowPosY = 0.0f;
    float initialScrollX = 0.0f;      // 创建时窗口滚动位置
    float initialScrollY = 0.0f;
    float initialBoundsX = 0.0f;      // 创建时控件的屏幕位置
    float initialBoundsY = 0.0f;
    bool active = false;
    bool fadeOut = false;  // 是否正在淡出
};

// Ripple 系统配置
struct RippleConfig {
    float expandDuration = 0.225f;  // 扩散持续时间 (秒)
    float fadeDuration = 0.150f;    // 淡出持续时间 (秒)
    float maxAlpha = 0.12f;         // 最大透明度
};

//=============================================================================
// 控件动画状态
//=============================================================================

// Toggle 开关动画状态
struct ToggleAnimState {
    SpringAnimator knobPosition;    // 旋钮位置 (0-1)
    SpringAnimator trackFill;       // 轨道填充 (0-1)
    SpringAnimator knobScale;       // 旋钮缩放
    SpringAnimator hoverState;      // 悬停状态

    ToggleAnimState()
        : knobPosition(0.0f, 300.0f, 22.0f)
        , trackFill(0.0f, 300.0f, 22.0f)
        , knobScale(1.0f, 400.0f, 25.0f)
        , hoverState(0.0f, 500.0f, 30.0f) {}
};

// Button 按钮动画状态
struct ButtonAnimState {
    SpringAnimator elevation;       // 高度/阴影
    SpringAnimator hoverState;      // 悬停状态
    SpringAnimator pressState;      // 按下状态

    ButtonAnimState()
        : elevation(0.0f, 400.0f, 28.0f)
        , hoverState(0.0f, 500.0f, 30.0f)
        , pressState(0.0f, 600.0f, 35.0f) {}
};

// Slider 滑块动画状态
struct SliderAnimState {
    SpringAnimator thumbScale;      // 滑块缩放
    SpringAnimator activeTrack;     // 活跃轨道长度
    SpringAnimator hoverState;      // 悬停状态

    SliderAnimState()
        : thumbScale(1.0f, 400.0f, 25.0f)
        , activeTrack(0.0f, 800.0f, 45.0f)  // 更快的弹簧：高刚度、高阻尼
        , hoverState(0.0f, 500.0f, 30.0f) {}
};

// Card 卡片动画状态
struct CardAnimState {
    SpringAnimator elevation;       // 高度
    SpringAnimator hoverState;      // 悬停状态

    CardAnimState()
        : elevation(1.0f, 300.0f, 25.0f)
        , hoverState(0.0f, 400.0f, 28.0f) {}
};

// Combo 下拉框动画状态
struct ComboAnimState {
    SpringAnimator hoverState;      // 悬停状态
    SpringAnimator openState;       // 展开状态 (0-1)
    SpringAnimator arrowRotation;   // 箭头旋转 (0-180度)
    float lastContentHeight = 0.0f; // 上次内容高度（用于动画）

    ComboAnimState()
        : hoverState(0.0f, 500.0f, 30.0f)
        , openState(0.0f, 800.0f, 40.0f)      // 更快的弹簧：高刚度、高阻尼
        , arrowRotation(0.0f, 800.0f, 40.0f)  // 箭头也加快
    {}
};

// CollapsingHeader 折叠头动画状态
struct CollapsingHeaderAnimState {
    SpringAnimator hoverState;      // 悬停状态
    SpringAnimator openState;       // 展开状态 (0-1)
    SpringAnimator arrowRotation;   // 箭头旋转 (0-90度)
    float lastContentHeight;        // 上一帧内容高度（用于动画）

    CollapsingHeaderAnimState()
        : hoverState(0.0f, 500.0f, 30.0f)
        , openState(0.0f, 350.0f, 26.0f)
        , arrowRotation(0.0f, 350.0f, 26.0f)
        , lastContentHeight(0.0f) {}
};

//=============================================================================
// MD3 上下文
//=============================================================================

// MD3 系统上下文（内部使用）
struct MD3Context {
    bool initialized = false;
    bool isDarkMode = true;
    float dpiScale = 1.0f;
    float deltaTime = 0.0f;
    float currentTime = 0.0f;

    // 色彩方案
    MD3ColorScheme colors;

    // Ripple 系统
    RippleConfig rippleConfig;
    std::vector<RippleState> ripples;
    GLuint rippleProgram = 0;
    GLuint rippleVAO = 0;
    GLuint rippleVBO = 0;

    // 控件动画状态缓存
    std::unordered_map<ImGuiID, ToggleAnimState> toggleStates;
    std::unordered_map<ImGuiID, ButtonAnimState> buttonStates;
    std::unordered_map<ImGuiID, SliderAnimState> sliderStates;
    std::unordered_map<ImGuiID, CardAnimState> cardStates;
    std::unordered_map<ImGuiID, ComboAnimState> comboStates;
    std::unordered_map<ImGuiID, CollapsingHeaderAnimState> collapsingHeaderStates;

    // 屏幕尺寸 (用于 Ripple shader)
    float screenWidth = 1920.0f;
    float screenHeight = 1080.0f;
};

// 获取全局上下文
MD3Context& GetContext();

//=============================================================================
// 公共 API
//=============================================================================

// 初始化 MD3 系统
void Init(float dpiScale = 1.0f);

// 关闭 MD3 系统
void Shutdown();

// 每帧开始时调用
void BeginFrame(float dt);

// 每帧结束时调用（渲染 Ripples）
void EndFrame();

// 设置深色/浅色模式
void SetDarkMode(bool dark);

// 获取当前模式
bool IsDarkMode();

// 设置屏幕尺寸
void SetScreenSize(float width, float height);

// 设置 DPI 缩放
void SetDpiScale(float scale);

//=============================================================================
// MD3 控件
//=============================================================================

// Toggle 开关
// 返回值：是否发生状态变化
bool Toggle(const char* label, bool* v);

// Filled Button（实心按钮）
bool FilledButton(const char* label, ImVec2 size = {0, 0});

// Tonal Button（调色按钮）
bool TonalButton(const char* label, ImVec2 size = {0, 0});

// Outlined Button（轮廓按钮）
bool OutlinedButton(const char* label, ImVec2 size = {0, 0});

// Text Button（文本按钮）
bool TextButton(const char* label);

// 默认 Button（等同于 FilledButton）
bool Button(const char* label, ImVec2 size = {0, 0});

// Slider 滑块
bool Slider(const char* label, float* v, float min, float max, const char* format = "%.1f");

// Card 卡片容器开始
// elevation: 0-5，控制阴影深度
bool BeginCard(const char* id, ImVec2 size = {0, 0}, int elevation = 1);

// Card 卡片容器结束
void EndCard();

//=============================================================================
// Combo 下拉框
//=============================================================================

// 开始下拉框
// preview_value: 当前显示的预览文本
bool BeginCombo(const char* label, const char* preview_value);

// 结束下拉框
void EndCombo();

// 下拉框选项
// selected: 是否为当前选中项（会显示勾选标记）
bool Selectable(const char* label, bool selected);

// 简化版下拉框（字符串数组）
bool Combo(const char* label, int* current_item, const char* const items[], int items_count);

//=============================================================================
// CollapsingHeader 折叠头
//=============================================================================

// 开始折叠头区域
// 返回 true 表示当前展开，内容应该被渲染
bool BeginCollapsingHeader(const char* label, bool default_open = false);

// 结束折叠头区域
void EndCollapsingHeader();

//=============================================================================
// Ripple 系统 API
//=============================================================================

// 触发 Ripple 效果
void TriggerRipple(ImGuiID id, float centerX, float centerY,
                   float boundsX, float boundsY, float boundsW, float boundsH,
                   float cornerRadius = 0.0f);

// 为当前控件触发 Ripple（使用 ImGui 上下文）
void TriggerRippleForCurrentItem(ImGuiID id, float cornerRadius = 0.0f);

// 绘制所有活跃的 Ripple（在控件绘制后调用）
// 使用 ImDrawList 渲染，自动跟随滚动位置
void DrawRipples();

//=============================================================================
// 工具函数
//=============================================================================

// 颜色混合
ImVec4 BlendColors(const ImVec4& base, const ImVec4& overlay, float alpha);

// 应用状态层
ImVec4 ApplyStateLayer(const ImVec4& base, const ImVec4& stateColor, float stateAlpha);

// 将 ImVec4 转换为 ImU32
unsigned int ColorToU32(const ImVec4& color);

// 从十六进制创建颜色
ImVec4 HexToColor(unsigned int hex, float alpha = 1.0f);

} // namespace MD3

//=============================================================================
// ImGui 集成钩子（供 ImGui patch 使用）
//=============================================================================

#ifdef IMGUI_MD3_ENABLED

// ImGui NewFrame 钩子
extern "C" void MD3_OnNewFrame(float dt);

// ImGui ButtonBehavior 钩子
extern "C" void MD3_TriggerRipple(unsigned int id, float mouseX, float mouseY,
                                   float bbMinX, float bbMinY, float bbMaxX, float bbMaxY);

// ImGui Checkbox 替换钩子
extern "C" bool MD3_Checkbox(const char* label, bool* v);

#endif
