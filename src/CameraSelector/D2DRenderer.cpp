#include "D2DRenderer.h"

#ifdef _WIN32

#include <iostream>

namespace CameraSelector {

Theme GetDarkTheme() {
    Theme theme;
    theme.background     = D2D1::ColorF(0x1C1C1C);      // 深灰背景
    theme.cardBackground = D2D1::ColorF(0x2D2D2D);      // 卡片背景
    theme.cardBorder     = D2D1::ColorF(0x404040);      // 卡片边框
    theme.cardHover      = D2D1::ColorF(0x505050);      // 悬停边框
    theme.cardSelected   = D2D1::ColorF(0x0078D4);      // 选中边框 (Windows 蓝)
    theme.textPrimary    = D2D1::ColorF(0xFFFFFF);      // 白色文字
    theme.textSecondary  = D2D1::ColorF(0xA0A0A0);      // 灰色文字
    theme.buttonBg       = D2D1::ColorF(0x0078D4);      // 按钮背景
    theme.buttonHover    = D2D1::ColorF(0x1084D8);      // 按钮悬停
    theme.buttonText     = D2D1::ColorF(0xFFFFFF);      // 按钮文字
    theme.accent         = D2D1::ColorF(0x0078D4);      // 主题色
    theme.checkboxBg     = D2D1::ColorF(0x3D3D3D);      // 复选框背景
    return theme;
}

Theme GetLightTheme() {
    Theme theme;
    theme.background     = D2D1::ColorF(0xF3F3F3);      // 浅灰背景
    theme.cardBackground = D2D1::ColorF(0xFFFFFF);      // 白色卡片
    theme.cardBorder     = D2D1::ColorF(0xE0E0E0);      // 浅边框
    theme.cardHover      = D2D1::ColorF(0xC0C0C0);      // 悬停边框
    theme.cardSelected   = D2D1::ColorF(0x0078D4);      // 选中边框
    theme.textPrimary    = D2D1::ColorF(0x1A1A1A);      // 深色文字
    theme.textSecondary  = D2D1::ColorF(0x666666);      // 灰色文字
    theme.buttonBg       = D2D1::ColorF(0x0078D4);      // 按钮背景
    theme.buttonHover    = D2D1::ColorF(0x1084D8);      // 按钮悬停
    theme.buttonText     = D2D1::ColorF(0xFFFFFF);      // 按钮文字
    theme.accent         = D2D1::ColorF(0x0078D4);      // 主题色
    theme.checkboxBg     = D2D1::ColorF(0xE8E8E8);      // 复选框背景
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
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndRtProps = D2D1::HwndRenderTargetProperties(hwnd, size);

    hr = m_factory->CreateHwndRenderTarget(rtProps, hwndRtProps, &m_renderTarget);
    if (FAILED(hr)) {
        std::cerr << "[D2DRenderer] Failed to create render target: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
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

} // namespace CameraSelector

#endif // _WIN32
