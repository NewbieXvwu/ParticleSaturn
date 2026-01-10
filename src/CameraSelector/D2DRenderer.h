#pragma once
// D2DRenderer - Direct2D 渲染封装
// 提供现代 UI 绘制能力：圆角矩形、阴影、文字、主题色等
// 包含 Ripple 效果和弹簧动画系统

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <d2d1_1.h>
#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <dwrite.h>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace CameraSelector {

//=============================================================================
// 弹簧动画系统
//=============================================================================

// 弹簧动画器 - 使用阻尼谐振器模型
struct SpringAnimator {
    float value     = 0.0f;
    float velocity  = 0.0f;
    float target    = 0.0f;
    float stiffness = 300.0f; // 刚度
    float damping   = 22.0f;  // 阻尼

    // 更新弹簧动画
    void Update(float dt) {
        if (!std::isfinite(dt) || dt <= 0.0f) {
            return;
        }
        if (!std::isfinite(value) || !std::isfinite(target) || !std::isfinite(velocity)) {
            if (!std::isfinite(target)) {
                target = 0.0f;
            }
            value    = target;
            velocity = 0.0f;
            return;
        }

        dt                       = (std::min)(dt, 0.5f);
        constexpr float kMaxStep = 1.0f / 120.0f;
        int             steps    = (dt > kMaxStep) ? (int)(dt / kMaxStep) + 1 : 1;
        steps                    = (std::min)(steps, 64);
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

    // 检查是否已稳定
    bool IsSettled(float threshold = 0.001f) const {
        return std::abs(target - value) < threshold && std::abs(velocity) < threshold;
    }

    // 立即跳转到目标值
    void SnapToTarget() {
        value    = target;
        velocity = 0.0f;
    }

    // 设置目标值
    void SetTarget(float t) { target = t; }

    SpringAnimator(float initial = 0.0f, float stiff = 300.0f, float damp = 22.0f)
        : value(initial), target(initial), stiffness(stiff), damping(damp) {}
};

//=============================================================================
// Ripple 效果状态
//=============================================================================

struct RippleState {
    float        centerX      = 0.0f;                              // 点击中心 X
    float        centerY      = 0.0f;                              // 点击中心 Y
    float        radius       = 0.0f;                              // 当前半径
    float        maxRadius    = 0.0f;                              // 最大半径
    float        alpha        = 0.0f;                              // 当前透明度
    float        maxAlpha     = 0.0f;                              // 峰值透明度（单次 Ripple）
    float        time         = 0.0f;                              // 动画时间
    D2D1_RECT_F  bounds       = {};                                // 边界矩形
    float        cornerRadius = 0.0f;                              // 圆角半径
    D2D1_COLOR_F color        = D2D1::ColorF(D2D1::ColorF::Black); // Ripple 颜色（不含 alpha）
    bool         active       = false;                             // 是否激活
    bool         fadeOut      = false;                             // 是否正在淡出

    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeometry;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush>      brush;
};

// Ripple 系统配置
struct RippleConfig {
    // 这套参数直接对齐你项目里 ImGui/MD3 的实现：expand + fadeOut 两段。
    float expandDuration = 0.270f; // 扩散持续时间 (秒) - 稍微慢一点更“稳”
    float fadeDuration   = 0.180f; // 淡出持续时间 (秒)
    float maxAlpha       = 0.14f;  // 峰值透明度（D2D 版稍微加一点，不然真看不见）

    float initialRadius  = 0.0f;  // 初始半径
    float maxRadiusScale = 1.00f; // 最大半径缩放系数

    // 软边缘宽度：对齐 MD3Shaders::FragmentRipple 里的 edgeWidth = max(20, radius * 0.15)
    float edgeMinPx = 20.0f;
    float edgeFrac  = 0.15f;
};

//=============================================================================
// 复选框动画状态
//=============================================================================

struct CheckboxAnimState {
    SpringAnimator checkProgress; // 勾选进度 (0-1)
    SpringAnimator hoverState;    // 悬停状态
    SpringAnimator pressState;    // 按下状态

    CheckboxAnimState()
        : checkProgress(0.0f, 400.0f, 28.0f), // 较高刚度，快速响应
          hoverState(0.0f, 500.0f, 30.0f),
          pressState(0.0f, 600.0f, 35.0f) {}
};

//=============================================================================
// 按钮动画状态
//=============================================================================

struct ButtonAnimState {
    SpringAnimator hoverState;
    SpringAnimator pressState;
    SpringAnimator elevation;

    ButtonAnimState()
        : hoverState(0.0f, 500.0f, 30.0f), pressState(0.0f, 600.0f, 35.0f), elevation(0.0f, 400.0f, 28.0f) {}
};

//=============================================================================
// 卡片动画状态
//=============================================================================

struct CardAnimState {
    SpringAnimator hoverState;
    SpringAnimator selectState;
    SpringAnimator elevation;

    CardAnimState()
        : hoverState(0.0f, 400.0f, 28.0f), selectState(0.0f, 350.0f, 25.0f), elevation(0.0f, 300.0f, 25.0f) {}
};

// 颜色主题
struct Theme {
    D2D1_COLOR_F background;                // 窗口背景
    D2D1_COLOR_F cardBackground;            // 卡片背景
    D2D1_COLOR_F cardBorder;                // 卡片边框
    D2D1_COLOR_F cardHover;                 // 卡片悬停边框
    D2D1_COLOR_F cardSelected;              // 卡片选中边框
    D2D1_COLOR_F textPrimary;               // 主要文字
    D2D1_COLOR_F textSecondary;             // 次要文字
    D2D1_COLOR_F buttonBg;                  // 按钮背景
    D2D1_COLOR_F buttonHover;               // 按钮悬停
    D2D1_COLOR_F buttonText;                // 按钮文字
    D2D1_COLOR_F accent;                    // 主题色
    D2D1_COLOR_F checkboxBg;                // 复选框背景
    D2D1_COLOR_F ripple;                    // Ripple 颜色
    float        stateLayerHover   = 0.08f; // 悬停状态层透明度
    float        stateLayerPressed = 0.12f; // 按下状态层透明度
};

// 获取深色/浅色主题
Theme GetDarkTheme();
Theme GetLightTheme();

class D2DRenderer {
  public:
    D2DRenderer();
    ~D2DRenderer();

    // 初始化 Direct2D 资源
    bool Initialize(HWND hwnd);

    // 释放资源
    void Release();

    // 开始/结束绘制
    void    BeginDraw();
    HRESULT EndDraw();

    // 清空背景
    void Clear(const D2D1_COLOR_F& color);

    // 绘制圆角矩形
    void DrawRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& fillColor,
                         const D2D1_COLOR_F* strokeColor = nullptr, float strokeWidth = 1.0f);

    // 绘制文字
    void DrawText(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float fontSize = 14.0f,
                  DWRITE_TEXT_ALIGNMENT      hAlign = DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                  DWRITE_FONT_WEIGHT         weight = DWRITE_FONT_WEIGHT_NORMAL);

    // 绘制位图
    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect, float opacity = 1.0f);

    // 绘制圆形
    void DrawCircle(float cx, float cy, float radius, const D2D1_COLOR_F& fillColor,
                    const D2D1_COLOR_F* strokeColor = nullptr, float strokeWidth = 1.0f);

    // 绘制复选框（带动画）
    void DrawCheckbox(const D2D1_RECT_F& rect, bool checked, const Theme& theme);

    // 绘制带动画的复选框
    void DrawAnimatedCheckbox(const D2D1_RECT_F& rect, CheckboxAnimState& state, bool checked, bool hovered,
                              bool pressed, const Theme& theme, float dt);

    // 绘制带动画的按钮
    void DrawAnimatedButton(const D2D1_RECT_F& rect, const std::wstring& text, ButtonAnimState& state, bool hovered,
                            bool pressed, const Theme& theme, float dt, bool primary = true);

    // 绘制带动画的卡片
    void DrawAnimatedCard(const D2D1_RECT_F& rect, CardAnimState& state, bool selected, bool hovered,
                          const Theme& theme, float dt);

    // Ripple 效果
    void TriggerRipple(float centerX, float centerY, const D2D1_RECT_F& bounds, float cornerRadius,
                       const D2D1_COLOR_F& color, float maxAlpha = -1.0f);
    void UpdateRipples(float dt);
    void DrawRipples();
    void ClearRipples();

    // 从 BGRA 像素数据创建位图
    ID2D1Bitmap* CreateBitmapFromPixels(const void* pixels, int width, int height);

    // 调整渲染目标大小
    void Resize(int width, int height);

    // 获取渲染目标
    ID2D1HwndRenderTarget* GetRenderTarget() const { return m_renderTarget; }

    // 获取 DWrite 工厂
    IDWriteFactory* GetDWriteFactory() const { return m_dwriteFactory; }

    // 工具函数
    static D2D1_COLOR_F BlendColors(const D2D1_COLOR_F& base, const D2D1_COLOR_F& overlay, float alpha);
    static D2D1_COLOR_F ApplyStateLayer(const D2D1_COLOR_F& base, const D2D1_COLOR_F& stateColor, float stateAlpha);

  private:
    ID2D1Factory*          m_factory       = nullptr;
    ID2D1HwndRenderTarget* m_renderTarget  = nullptr;
    IDWriteFactory*        m_dwriteFactory = nullptr;
    HWND                   m_hwnd          = nullptr;

    // Ripple 系统
    std::vector<RippleState> m_ripples;
    RippleConfig             m_rippleConfig;
    ID2D1Layer*              m_rippleLayer = nullptr;

    // 绘制 Ripple 辅助函数
    void DrawSingleRipple(const RippleState& ripple);
};

} // namespace CameraSelector

#endif // _WIN32
