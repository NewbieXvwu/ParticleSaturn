#pragma once
// D2DRenderer - Direct2D 渲染封装
// 提供现代 UI 绘制能力：圆角矩形、阴影、文字、主题色等

#ifdef _WIN32

#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <memory>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace CameraSelector {

// 颜色主题
struct Theme {
    D2D1_COLOR_F background;      // 窗口背景
    D2D1_COLOR_F cardBackground;  // 卡片背景
    D2D1_COLOR_F cardBorder;      // 卡片边框
    D2D1_COLOR_F cardHover;       // 卡片悬停边框
    D2D1_COLOR_F cardSelected;    // 卡片选中边框
    D2D1_COLOR_F textPrimary;     // 主要文字
    D2D1_COLOR_F textSecondary;   // 次要文字
    D2D1_COLOR_F buttonBg;        // 按钮背景
    D2D1_COLOR_F buttonHover;     // 按钮悬停
    D2D1_COLOR_F buttonText;      // 按钮文字
    D2D1_COLOR_F accent;          // 主题色
    D2D1_COLOR_F checkboxBg;      // 复选框背景
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
    void BeginDraw();
    HRESULT EndDraw();

    // 清空背景
    void Clear(const D2D1_COLOR_F& color);

    // 绘制圆角矩形
    void DrawRoundedRect(const D2D1_RECT_F& rect, float radius,
                         const D2D1_COLOR_F& fillColor,
                         const D2D1_COLOR_F* strokeColor = nullptr,
                         float strokeWidth = 1.0f);

    // 绘制文字
    void DrawText(const std::wstring& text, const D2D1_RECT_F& rect,
                  const D2D1_COLOR_F& color, float fontSize = 14.0f,
                  DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_CENTER,
                  DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // 绘制位图
    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect,
                    float opacity = 1.0f);

    // 绘制圆形
    void DrawCircle(float cx, float cy, float radius,
                    const D2D1_COLOR_F& fillColor,
                    const D2D1_COLOR_F* strokeColor = nullptr,
                    float strokeWidth = 1.0f);

    // 绘制复选框
    void DrawCheckbox(const D2D1_RECT_F& rect, bool checked,
                      const Theme& theme);

    // 从 BGRA 像素数据创建位图
    ID2D1Bitmap* CreateBitmapFromPixels(const void* pixels, int width, int height);

    // 调整渲染目标大小
    void Resize(int width, int height);

    // 获取渲染目标
    ID2D1HwndRenderTarget* GetRenderTarget() const { return m_renderTarget; }

    // 获取 DWrite 工厂
    IDWriteFactory* GetDWriteFactory() const { return m_dwriteFactory; }

private:
    ID2D1Factory*          m_factory       = nullptr;
    ID2D1HwndRenderTarget* m_renderTarget  = nullptr;
    IDWriteFactory*        m_dwriteFactory = nullptr;
    HWND                   m_hwnd          = nullptr;
};

} // namespace CameraSelector

#endif // _WIN32
