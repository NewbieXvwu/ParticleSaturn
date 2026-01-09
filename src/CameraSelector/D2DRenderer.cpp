#include "D2DRenderer.h"

#ifdef _WIN32

#include <iostream>
#include <utility>

namespace CameraSelector {

Theme GetDarkTheme() {
    Theme theme;
    theme.background     = D2D1::ColorF(0x1C1C1C, 0.0f);  // 透明背景用于 Mica
    theme.cardBackground = D2D1::ColorF(0x2D2D2D, 0.85f); // 半透明卡片
    theme.cardBorder     = D2D1::ColorF(0x404040);        // 卡片边框
    theme.cardHover      = D2D1::ColorF(0x505050);        // 悬停边框
    theme.cardSelected   = D2D1::ColorF(0x0078D4);        // 选中边框 (Windows 蓝)
    theme.textPrimary    = D2D1::ColorF(0xFFFFFF);        // 白色文字
    theme.textSecondary  = D2D1::ColorF(0xA0A0A0);        // 灰色文字
    theme.buttonBg       = D2D1::ColorF(0x0078D4);        // 按钮背景
    theme.buttonHover    = D2D1::ColorF(0x1084D8);        // 按钮悬停
    theme.buttonText     = D2D1::ColorF(0xFFFFFF);        // 按钮文字
    theme.accent         = D2D1::ColorF(0x0078D4);        // 主题色
    theme.checkboxBg     = D2D1::ColorF(0x3D3D3D);        // 复选框背景
    theme.ripple         = D2D1::ColorF(0xFFFFFF);        // Ripple 颜色（深色模式用白色）
    theme.stateLayerHover = 0.08f;
    theme.stateLayerPressed = 0.12f;
    return theme;
}

Theme GetLightTheme() {
    Theme theme;
    theme.background     = D2D1::ColorF(0xF3F3F3, 0.0f);  // 透明背景用于 Mica
    theme.cardBackground = D2D1::ColorF(0xFFFFFF, 0.90f); // 半透明白色卡片
    theme.cardBorder     = D2D1::ColorF(0xE0E0E0);        // 浅边框
    theme.cardHover      = D2D1::ColorF(0xC0C0C0);        // 悬停边框
    theme.cardSelected   = D2D1::ColorF(0x0078D4);        // 选中边框
    theme.textPrimary    = D2D1::ColorF(0x1A1A1A);        // 深色文字
    theme.textSecondary  = D2D1::ColorF(0x666666);        // 灰色文字
    theme.buttonBg       = D2D1::ColorF(0x0078D4);        // 按钮背景
    theme.buttonHover    = D2D1::ColorF(0x1084D8);        // 按钮悬停
    theme.buttonText     = D2D1::ColorF(0xFFFFFF);        // 按钮文字
    theme.accent         = D2D1::ColorF(0x0078D4);        // 主题色
    theme.checkboxBg     = D2D1::ColorF(0xE8E8E8);        // 复选框背景
    theme.ripple         = D2D1::ColorF(0x000000);        // Ripple 颜色（浅色模式用黑色）
    theme.stateLayerHover = 0.08f;
    theme.stateLayerPressed = 0.12f;
    return theme;
}

D2DRenderer::D2DRenderer() {}

D2DRenderer::~D2DRenderer() {
    Release();
}

bool D2DRenderer::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    // 创建 D2D 工厂
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory);
    if (FAILED(hr)) {
        std::cerr << "[D2DRenderer] Failed to create D2D factory: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 创建渲染目标
    RECT rc;
    GetClientRect(hwnd, &rc);

    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndRtProps = D2D1::HwndRenderTargetProperties(hwnd, size);

    hr = m_factory->CreateHwndRenderTarget(rtProps, hwndRtProps, &m_renderTarget);
    if (FAILED(hr)) {
        std::cerr << "[D2DRenderer] Failed to create render target: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Ripple layer（复用，避免每帧/每个 ripple 乱创建 COM 对象）
    hr = m_renderTarget->CreateLayer(&m_rippleLayer);
    if (FAILED(hr)) {
        m_rippleLayer = nullptr;
    }

    // 创建 DWrite 工厂
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&m_dwriteFactory));
    if (FAILED(hr)) {
        std::cerr << "[D2DRenderer] Failed to create DWrite factory: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[D2DRenderer] Initialized successfully" << std::endl;
    return true;
}

void D2DRenderer::Release() {
    ClearRipples();

    if (m_rippleLayer) {
        m_rippleLayer->Release();
        m_rippleLayer = nullptr;
    }
    if (m_dwriteFactory) {
        m_dwriteFactory->Release();
        m_dwriteFactory = nullptr;
    }
    if (m_renderTarget) {
        m_renderTarget->Release();
        m_renderTarget = nullptr;
    }
    if (m_factory) {
        m_factory->Release();
        m_factory = nullptr;
    }
}

void D2DRenderer::BeginDraw() {
    if (m_renderTarget) {
        m_renderTarget->BeginDraw();
    }
}

HRESULT D2DRenderer::EndDraw() {
    if (m_renderTarget) {
        return m_renderTarget->EndDraw();
    }
    return E_FAIL;
}

void D2DRenderer::Clear(const D2D1_COLOR_F& color) {
    if (m_renderTarget) {
        m_renderTarget->Clear(color);
    }
}

void D2DRenderer::DrawRoundedRect(const D2D1_RECT_F& rect, float radius,
                                   const D2D1_COLOR_F& fillColor,
                                   const D2D1_COLOR_F* strokeColor,
                                   float strokeWidth) {
    if (!m_renderTarget) return;

    ID2D1SolidColorBrush* brush = nullptr;
    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(rect, radius, radius);

    // 填充
    m_renderTarget->CreateSolidColorBrush(fillColor, &brush);
    if (brush) {
        m_renderTarget->FillRoundedRectangle(roundedRect, brush);
        brush->Release();
    }

    // 边框
    if (strokeColor) {
        m_renderTarget->CreateSolidColorBrush(*strokeColor, &brush);
        if (brush) {
            m_renderTarget->DrawRoundedRectangle(roundedRect, brush, strokeWidth);
            brush->Release();
        }
    }
}

void D2DRenderer::DrawText(const std::wstring& text, const D2D1_RECT_F& rect,
                            const D2D1_COLOR_F& color, float fontSize,
                            DWRITE_TEXT_ALIGNMENT hAlign,
                            DWRITE_PARAGRAPH_ALIGNMENT vAlign,
                            DWRITE_FONT_WEIGHT weight) {
    if (!m_renderTarget || !m_dwriteFactory) return;

    IDWriteTextFormat* textFormat = nullptr;
    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        weight,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"",
        &textFormat
    );

    if (SUCCEEDED(hr) && textFormat) {
        textFormat->SetTextAlignment(hAlign);
        textFormat->SetParagraphAlignment(vAlign);

        ID2D1SolidColorBrush* brush = nullptr;
        m_renderTarget->CreateSolidColorBrush(color, &brush);
        if (brush) {
            m_renderTarget->DrawTextW(text.c_str(), static_cast<UINT32>(text.length()),
                                       textFormat, rect, brush);
            brush->Release();
        }
        textFormat->Release();
    }
}

void D2DRenderer::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect,
                              float opacity) {
    if (!m_renderTarget || !bitmap) return;
    m_renderTarget->DrawBitmap(bitmap, destRect, opacity);
}

void D2DRenderer::DrawCircle(float cx, float cy, float radius,
                              const D2D1_COLOR_F& fillColor,
                              const D2D1_COLOR_F* strokeColor,
                              float strokeWidth) {
    if (!m_renderTarget) return;

    D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
    ID2D1SolidColorBrush* brush = nullptr;

    // 填充
    m_renderTarget->CreateSolidColorBrush(fillColor, &brush);
    if (brush) {
        m_renderTarget->FillEllipse(ellipse, brush);
        brush->Release();
    }

    // 边框
    if (strokeColor) {
        m_renderTarget->CreateSolidColorBrush(*strokeColor, &brush);
        if (brush) {
            m_renderTarget->DrawEllipse(ellipse, brush, strokeWidth);
            brush->Release();
        }
    }
}

void D2DRenderer::DrawCheckbox(const D2D1_RECT_F& rect, bool checked,
                                const Theme& theme) {
    float size = rect.bottom - rect.top;
    float radius = size * 0.15f;

    if (checked) {
        // 选中状态：填充主题色
        DrawRoundedRect(rect, radius, theme.accent, nullptr);

        // 绘制勾号
        ID2D1SolidColorBrush* brush = nullptr;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        if (brush) {
            float cx = (rect.left + rect.right) / 2;
            float cy = (rect.top + rect.bottom) / 2;
            float s = size * 0.25f;

            ID2D1PathGeometry* path = nullptr;
            m_factory->CreatePathGeometry(&path);
            if (path) {
                ID2D1GeometrySink* sink = nullptr;
                path->Open(&sink);
                if (sink) {
                    sink->BeginFigure(D2D1::Point2F(cx - s, cy), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(cx - s * 0.3f, cy + s * 0.7f));
                    sink->AddLine(D2D1::Point2F(cx + s, cy - s * 0.5f));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    sink->Release();

                    m_renderTarget->DrawGeometry(path, brush, 2.0f);
                }
                path->Release();
            }
            brush->Release();
        }
    } else {
        // 未选中状态：只有边框
        DrawRoundedRect(rect, radius, theme.checkboxBg, &theme.cardBorder, 1.5f);
    }
}

ID2D1Bitmap* D2DRenderer::CreateBitmapFromPixels(const void* pixels, int width, int height) {
    if (!m_renderTarget || !pixels) return nullptr;

    D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    ID2D1Bitmap* bitmap = nullptr;
    HRESULT hr = m_renderTarget->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels,
        width * 4,
        bitmapProps,
        &bitmap
    );

    if (FAILED(hr)) {
        std::cerr << "[D2DRenderer] Failed to create bitmap: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return nullptr;
    }

    return bitmap;
}

void D2DRenderer::Resize(int width, int height) {
    if (m_renderTarget) {
        m_renderTarget->Resize(D2D1::SizeU(width, height));
    }
}

//=============================================================================
// 工具函数
//=============================================================================

D2D1_COLOR_F D2DRenderer::BlendColors(const D2D1_COLOR_F& base, const D2D1_COLOR_F& overlay, float alpha) {
    alpha = (std::max)(0.0f, (std::min)(1.0f, alpha));
    return D2D1::ColorF(
        base.r + (overlay.r - base.r) * alpha,
        base.g + (overlay.g - base.g) * alpha,
        base.b + (overlay.b - base.b) * alpha,
        base.a + (overlay.a - base.a) * alpha
    );
}

D2D1_COLOR_F D2DRenderer::ApplyStateLayer(const D2D1_COLOR_F& base, const D2D1_COLOR_F& stateColor, float stateAlpha) {
    stateAlpha = (std::max)(0.0f, (std::min)(1.0f, stateAlpha));
    return D2D1::ColorF(
        base.r + (stateColor.r - base.r) * stateAlpha,
        base.g + (stateColor.g - base.g) * stateAlpha,
        base.b + (stateColor.b - base.b) * stateAlpha,
        base.a
    );
}

//=============================================================================
// Ripple 效果实现
//=============================================================================

namespace {
static float Clamp01(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x;
}
} // namespace

void D2DRenderer::TriggerRipple(float centerX, float centerY,
                                const D2D1_RECT_F& bounds, float cornerRadius,
                                const D2D1_COLOR_F& color, float maxAlpha) {
    if (!m_factory || !m_renderTarget) return;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;

    RippleState ripple;
    ripple.centerX = centerX;
    ripple.centerY = centerY;
    ripple.bounds = bounds;
    ripple.cornerRadius = cornerRadius;
    ripple.radius = (std::max)(0.0f, m_rippleConfig.initialRadius);
    ripple.time = 0.0f;
    ripple.active = true;
    ripple.fadeOut = false;

    ripple.color = D2D1_COLOR_F{color.r, color.g, color.b, 1.0f};
    ripple.maxAlpha = (maxAlpha >= 0.0f) ? maxAlpha : m_rippleConfig.maxAlpha;
    ripple.maxAlpha = Clamp01(ripple.maxAlpha);
    ripple.alpha = 0.0f;

    // 计算最大半径：到最远角的距离（允许点击点略微越界）
    float farX = (std::max)(std::abs(centerX - bounds.left), std::abs(bounds.right - centerX));
    float farY = (std::max)(std::abs(centerY - bounds.top), std::abs(bounds.bottom - centerY));
    ripple.maxRadius = std::sqrt(farX * farX + farY * farY) * (std::max)(0.1f, m_rippleConfig.maxRadiusScale);

    // 裁剪几何体（只创建一次，别每帧都 Create* 然后装死）
    HRESULT hr = m_factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(bounds, cornerRadius, cornerRadius),
        ripple.clipGeometry.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr) || !ripple.clipGeometry) return;

    // Brush：用 radial gradient 做软边缘（否则就是 2005 年的“纯色圆”）
    float edgeMinPx = (std::max)(0.0f, m_rippleConfig.edgeMinPx);
    float edgeFrac = (std::max)(0.0f, m_rippleConfig.edgeFrac);
    float edgeWidth = (std::max)(edgeMinPx, ripple.maxRadius * edgeFrac);
    float softEdgeStart = (ripple.maxRadius > 0.001f) ? Clamp01((ripple.maxRadius - edgeWidth) / ripple.maxRadius)
                                                      : 0.0f;
    D2D1_COLOR_F center = D2D1_COLOR_F{ripple.color.r, ripple.color.g, ripple.color.b, 1.0f};
    D2D1_COLOR_F edge = D2D1_COLOR_F{ripple.color.r, ripple.color.g, ripple.color.b, 0.0f};
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, center},
        {softEdgeStart, center},
        {1.0f, edge},
    };

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection;
    hr = m_renderTarget->CreateGradientStopCollection(
        stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCollection.GetAddressOf()
    );
    if (FAILED(hr) || !stopCollection) return;

    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = D2D1::RadialGradientBrushProperties(
        D2D1::Point2F(centerX, centerY),
        D2D1::Point2F(0.0f, 0.0f),
        (std::max)(1.0f, ripple.radius),
        (std::max)(1.0f, ripple.radius)
    );

    hr = m_renderTarget->CreateRadialGradientBrush(
        &props, nullptr, stopCollection.Get(), ripple.brush.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr) || !ripple.brush) return;

    m_ripples.push_back(std::move(ripple));
}

void D2DRenderer::UpdateRipples(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    dt = (std::min)(dt, 0.25f);

    float expandDuration = (std::max)(0.001f, m_rippleConfig.expandDuration);
    float fadeDuration = (std::max)(0.001f, m_rippleConfig.fadeDuration);

    for (auto& ripple : m_ripples) {
        if (!ripple.active) continue;

        ripple.time += dt;

        if (!ripple.fadeOut) {
            float expandProgress = ripple.time / expandDuration;
            if (expandProgress >= 1.0f) {
                expandProgress = 1.0f;
                ripple.fadeOut = true;
                ripple.time = 0.0f;
            }

            // 对齐 MD3Context.cpp：ease-out quadratic（不是 cubic 那种黏糊糊的）
            float eased = 1.0f - (1.0f - Clamp01(expandProgress)) * (1.0f - Clamp01(expandProgress));
            float startRadius = (std::max)(0.0f, m_rippleConfig.initialRadius);
            ripple.radius = startRadius + (ripple.maxRadius - startRadius) * eased;
            ripple.alpha = ripple.maxAlpha;
        } else {
            float fadeProgress = ripple.time / fadeDuration;
            if (fadeProgress >= 1.0f) {
                fadeProgress = 1.0f;
                ripple.active = false;
            }

            ripple.radius = ripple.maxRadius;
            ripple.alpha = ripple.maxAlpha * (1.0f - Clamp01(fadeProgress));
        }
    }

    // 移除已完成的 ripple
    m_ripples.erase(
        std::remove_if(m_ripples.begin(), m_ripples.end(),
                       [](const RippleState& r) { return !r.active; }),
        m_ripples.end()
    );
}

void D2DRenderer::DrawSingleRipple(const RippleState& ripple) {
    if (!m_renderTarget || !m_factory) return;
    if (ripple.alpha <= 0.0f || ripple.radius <= 0.0f) return;
    if (!ripple.clipGeometry) return;

    if (!m_rippleLayer) {
        HRESULT hr = m_renderTarget->CreateLayer(&m_rippleLayer);
        if (FAILED(hr)) return;
    }

    m_renderTarget->PushLayer(
        D2D1::LayerParameters(D2D1::InfiniteRect(), ripple.clipGeometry.Get()),
        m_rippleLayer
    );

    if (ripple.brush) {
        ripple.brush->SetCenter(D2D1::Point2F(ripple.centerX, ripple.centerY));
        ripple.brush->SetRadiusX(ripple.radius);
        ripple.brush->SetRadiusY(ripple.radius);
        ripple.brush->SetOpacity(ripple.alpha);

        D2D1_ELLIPSE ellipse = D2D1::Ellipse(
            D2D1::Point2F(ripple.centerX, ripple.centerY),
            ripple.radius, ripple.radius
        );
        m_renderTarget->FillEllipse(ellipse, ripple.brush.Get());
    } else {
        // 极端 fallback：没有 gradient brush 就用纯色，至少别崩。
        ID2D1SolidColorBrush* brush = nullptr;
        D2D1_COLOR_F c = ripple.color;
        c.a = ripple.alpha;
        m_renderTarget->CreateSolidColorBrush(c, &brush);
        if (brush) {
            D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                D2D1::Point2F(ripple.centerX, ripple.centerY),
                ripple.radius, ripple.radius
            );
            m_renderTarget->FillEllipse(ellipse, brush);
            brush->Release();
        }
    }

    m_renderTarget->PopLayer();
}

void D2DRenderer::DrawRipples() {
    for (const auto& ripple : m_ripples) {
        if (ripple.active) {
            DrawSingleRipple(ripple);
        }
    }
}

void D2DRenderer::ClearRipples() {
    m_ripples.clear();
}

//=============================================================================
// 带动画的复选框
//=============================================================================

void D2DRenderer::DrawAnimatedCheckbox(const D2D1_RECT_F& rect,
                                        CheckboxAnimState& state,
                                        bool checked, bool hovered, bool pressed,
                                        const Theme& theme, float dt) {
    // 更新动画状态
    state.checkProgress.SetTarget(checked ? 1.0f : 0.0f);
    state.hoverState.SetTarget(hovered ? 1.0f : 0.0f);
    state.pressState.SetTarget(pressed ? 1.0f : 0.0f);

    state.checkProgress.Update(dt);
    state.hoverState.Update(dt);
    state.pressState.Update(dt);

    float checkProgress = (std::max)(0.0f, (std::min)(1.0f, state.checkProgress.value));
    float hoverVal = state.hoverState.value;
    float pressVal = state.pressState.value;
    float scale = 1.0f; // 复选框大小不要抖，别搞“点击后弹跳缩放”那套花活。

    // 计算尺寸和位置
    float size = rect.bottom - rect.top;
    float cx = (rect.left + rect.right) / 2.0f;
    float cy = (rect.top + rect.bottom) / 2.0f;
    float radius = size * 0.15f;

    // 应用缩放
    float scaledSize = size * scale;
    D2D1_RECT_F scaledRect = D2D1::RectF(
        cx - scaledSize / 2.0f,
        cy - scaledSize / 2.0f,
        cx + scaledSize / 2.0f,
        cy + scaledSize / 2.0f
    );

    // 背景色：在未选中和选中状态之间混合
    D2D1_COLOR_F bgColor = BlendColors(theme.checkboxBg, theme.accent, checkProgress);

    // 应用悬停/按下状态层
    float stateLayer = hoverVal * theme.stateLayerHover + pressVal * theme.stateLayerPressed;
    bgColor = ApplyStateLayer(bgColor, theme.ripple, stateLayer);

    // 绘制背景（别用 checkProgress 做“有/无边框”的硬切换：弹簧会在 1.0 附近来回抖，
    // 你就会看到边框出现/消失导致的“尺寸跳动”。用连续的 alpha 淡出解决边界情况。）
    float borderAlpha = 1.0f - checkProgress;
    if (borderAlpha > 0.001f) {
        D2D1_COLOR_F borderColor = BlendColors(theme.cardBorder, theme.accent, checkProgress);
        borderColor.a *= borderAlpha;
        DrawRoundedRect(scaledRect, radius * scale, bgColor, &borderColor, 1.5f);
    } else {
        DrawRoundedRect(scaledRect, radius * scale, bgColor, nullptr);
    }

    // 绘制勾号（渐进式绘制）
    if (checkProgress > 0.01f) {
        ID2D1SolidColorBrush* brush = nullptr;
        D2D1_COLOR_F checkColor = D2D1::ColorF(D2D1::ColorF::White, checkProgress);
        m_renderTarget->CreateSolidColorBrush(checkColor, &brush);

        if (brush) {
            float s = scaledSize * 0.25f;

            // 勾号的三个点
            D2D1_POINT_2F p1 = D2D1::Point2F(cx - s, cy);
            D2D1_POINT_2F p2 = D2D1::Point2F(cx - s * 0.3f, cy + s * 0.7f);
            D2D1_POINT_2F p3 = D2D1::Point2F(cx + s, cy - s * 0.5f);

            ID2D1PathGeometry* path = nullptr;
            m_factory->CreatePathGeometry(&path);
            if (path) {
                ID2D1GeometrySink* sink = nullptr;
                path->Open(&sink);
                if (sink) {
                    // 根据进度渐进绘制
                    if (checkProgress <= 0.5f) {
                        // 只绘制第一段
                        float seg1Progress = checkProgress * 2.0f;
                        D2D1_POINT_2F endPt = D2D1::Point2F(
                            p1.x + (p2.x - p1.x) * seg1Progress,
                            p1.y + (p2.y - p1.y) * seg1Progress
                        );
                        sink->BeginFigure(p1, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(endPt);
                    } else {
                        // 第一段完成，绘制第二段
                        float seg2Progress = (checkProgress - 0.5f) * 2.0f;
                        D2D1_POINT_2F endPt = D2D1::Point2F(
                            p2.x + (p3.x - p2.x) * seg2Progress,
                            p2.y + (p3.y - p2.y) * seg2Progress
                        );
                        sink->BeginFigure(p1, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p2);
                        sink->AddLine(endPt);
                    }
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    sink->Release();

                    m_renderTarget->DrawGeometry(path, brush, 2.0f * scale);
                }
                path->Release();
            }
            brush->Release();
        }
    }
}

//=============================================================================
// 带动画的按钮
//=============================================================================

void D2DRenderer::DrawAnimatedButton(const D2D1_RECT_F& rect,
                                      const std::wstring& text,
                                      ButtonAnimState& state,
                                      bool hovered, bool pressed,
                                      const Theme& theme, float dt,
                                      bool primary) {
    // 更新动画
    state.hoverState.SetTarget(hovered ? 1.0f : 0.0f);
    state.pressState.SetTarget(pressed ? 1.0f : 0.0f);

    state.hoverState.Update(dt);
    state.pressState.Update(dt);

    float hoverVal = state.hoverState.value;
    float pressVal = state.pressState.value;

    float radius = 6.0f;

    if (primary) {
        // 主按钮：主题色填充
        D2D1_COLOR_F bgColor = BlendColors(theme.buttonBg, theme.buttonHover, hoverVal);

        // 按下时稍微变暗
        if (pressVal > 0.0f) {
            D2D1_COLOR_F pressedColor = D2D1::ColorF(
                bgColor.r * 0.85f, bgColor.g * 0.85f, bgColor.b * 0.85f, bgColor.a
            );
            bgColor = BlendColors(bgColor, pressedColor, pressVal);
        }

        DrawRoundedRect(rect, radius, bgColor, nullptr);
        DrawText(text, rect, theme.buttonText, 14.0f,
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                 DWRITE_FONT_WEIGHT_SEMI_BOLD);
        return;
    }

    // 次按钮：卡片风格 + 边框
    D2D1_COLOR_F bgColor = theme.cardBackground;
    bgColor.a = (std::max)(bgColor.a, 0.90f);

    float stateLayer = hoverVal * theme.stateLayerHover + pressVal * theme.stateLayerPressed;
    bgColor = ApplyStateLayer(bgColor, theme.ripple, stateLayer);

    D2D1_COLOR_F borderColor = BlendColors(theme.cardBorder, theme.cardHover, hoverVal);
    DrawRoundedRect(rect, radius, bgColor, &borderColor, 1.0f);

    DrawText(text, rect, theme.textPrimary, 14.0f,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

//=============================================================================
// 带动画的卡片
//=============================================================================

void D2DRenderer::DrawAnimatedCard(const D2D1_RECT_F& rect,
                                    CardAnimState& state,
                                    bool selected, bool hovered,
                                    const Theme& theme, float dt) {
    // 更新动画
    state.hoverState.SetTarget(hovered ? 1.0f : 0.0f);
    state.selectState.SetTarget(selected ? 1.0f : 0.0f);

    state.hoverState.Update(dt);
    state.selectState.Update(dt);

    float hoverVal = state.hoverState.value;
    float selectVal = state.selectState.value;

    // 卡片背景色 - 悬停时稍微变亮
    D2D1_COLOR_F bgColor = theme.cardBackground;
    float stateLayer = hoverVal * theme.stateLayerHover;
    bgColor = ApplyStateLayer(bgColor, theme.ripple, stateLayer);

    // 边框色：根据选中和悬停状态变化
    D2D1_COLOR_F borderColor;
    if (selectVal > 0.5f) {
        borderColor = BlendColors(theme.cardHover, theme.cardSelected, (selectVal - 0.5f) * 2.0f);
    } else {
        borderColor = BlendColors(theme.cardBorder, theme.cardHover, hoverVal);
    }

    // 绘制卡片
    float radius = 8.0f;
    float borderWidth = 1.0f + selectVal * 1.0f; // 选中时边框加粗
    DrawRoundedRect(rect, radius, bgColor, &borderColor, borderWidth);
}

} // namespace CameraSelector

#endif // _WIN32
