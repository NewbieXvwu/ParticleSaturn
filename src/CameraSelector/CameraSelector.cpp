#include "CameraSelector.h"
#include "CameraEnumerator.h"
#include "CameraPreview.h"
#include "D2DRenderer.h"

#ifdef _WIN32

#include <dwmapi.h>
#include <iostream>
#include <map>

#pragma comment(lib, "dwmapi.lib")

namespace CameraSelector {

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

static constexpr DWORD kCornerPrefRound         = 2;
static constexpr int   kBackdropMainWindow      = 2;
static constexpr int   kBackdropTransientWindow = 3;

// 注册表路径
static const wchar_t* REGISTRY_KEY = L"SOFTWARE\\ParticleSaturn";
static const wchar_t* REGISTRY_VALUE = L"SelectedCamera";
static const wchar_t* REGISTRY_VALUE_SKIP = L"SkipCameraSelectorDialog";

// 窗口类名
static const wchar_t* WINDOW_CLASS = L"CameraSelectorWindow";

// UI 常量
static constexpr int CARD_WIDTH = 340;
static constexpr int CARD_HEIGHT = 300;
static constexpr int CARD_PADDING = 20;
static constexpr int PREVIEW_WIDTH = 320;
static constexpr int PREVIEW_HEIGHT = 240;
static constexpr int CARD_SPACING = 20;
static constexpr int TITLE_HEIGHT = 72;
static constexpr int BUTTON_HEIGHT = 40;
static constexpr int BUTTON_WIDTH = 120;
static constexpr int BUTTON_GAP = 12;
static constexpr int CHECKBOX_SIZE = 20;
static constexpr int BOTTOM_PADDING = 112;
static constexpr int BUTTON_BOTTOM_MARGIN = 18;
static constexpr int CHECKBOX_ROW_TOP = 14;

// 对话框状态
struct DialogState {
    std::vector<CameraInfo> cameras;
    CameraPreviewManager previewManager;
    D2DRenderer renderer;
    Theme theme;

    int selectedIndex = 0;
    int hoverIndex = -1;
    bool rememberChoice = false;
    bool checkboxHover = false;
    bool checkboxPressed = false;
    bool okHover = false;
    bool okPressed = false;
    bool cancelHover = false;
    bool cancelPressed = false;
    bool confirmed = false;
    bool isDarkMode = true;

    // 动画状态
    std::vector<CardAnimState> cardAnims;
    CheckboxAnimState checkboxAnim;
    ButtonAnimState okButtonAnim;
    ButtonAnimState cancelButtonAnim;

    // 时间跟踪
    LARGE_INTEGER lastTime = {};
    LARGE_INTEGER frequency = {};
    float dt = 0.0f;

    // 缓存的位图
    std::map<int, ID2D1Bitmap*> cachedBitmaps;

    HWND hwnd = nullptr;
    int windowWidth = 0;
    int windowHeight = 0;
};

// 检测系统深色模式
static bool IsSystemDarkMode() {
    auto IsColorDark = [](COLORREF c) -> bool {
        // Perceived luminance (sRGB-ish). We just need a stable heuristic for legacy OSes.
        const double r = static_cast<double>(GetRValue(c)) / 255.0;
        const double g = static_cast<double>(GetGValue(c)) / 255.0;
        const double b = static_cast<double>(GetBValue(c)) / 255.0;
        const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        return luminance < 0.5;
    };

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size  = sizeof(value);
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return value == 0;
        }
        RegCloseKey(hKey);
    }

    // Win7 / no-personalize-key fallback: infer from system window background color.
    // Win7 has no global "apps dark mode", so defaulting to dark makes the UI look mismatched.
    return IsColorDark(GetSysColor(COLOR_WINDOW));
}

static void TrySetTitleBarDarkMode(HWND hwnd, bool darkMode) {
    BOOL useDark = darkMode ? TRUE : FALSE;
    HRESULT hr   = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    if (FAILED(hr)) {
        (void)DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
    }
}

static void TrySetRoundedCorners(HWND hwnd) {
    DWORD cornerPref = kCornerPrefRound;
    (void)DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
}

static void ExtendDwmFrameToClient(HWND hwnd) {
    MARGINS margins = {-1, -1, -1, -1};
    (void)DwmExtendFrameIntoClientArea(hwnd, &margins);
}

static HRESULT SetSystemBackdrop(HWND hwnd, int backdropType) {
    ExtendDwmFrameToClient(hwnd);

    return DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
}

static bool TryEnableAeroBlur(HWND hwnd) {
    BOOL compositionEnabled = FALSE;
    HRESULT hr              = DwmIsCompositionEnabled(&compositionEnabled);
    if (FAILED(hr) || !compositionEnabled) {
        return false;
    }

    ExtendDwmFrameToClient(hwnd);

    DWM_BLURBEHIND bb = {};
    bb.dwFlags        = DWM_BB_ENABLE;
    bb.fEnable        = TRUE;
    hr                = DwmEnableBlurBehindWindow(hwnd, &bb);
    return SUCCEEDED(hr);
}

static void ApplyBackdropStyle(HWND hwnd, bool darkMode) {
    TrySetTitleBarDarkMode(hwnd, darkMode);
    TrySetRoundedCorners(hwnd);

    HRESULT hr = SetSystemBackdrop(hwnd, kBackdropMainWindow);
    if (SUCCEEDED(hr)) {
        std::cout << "[CameraSelector] Backdrop: Mica (DWMSBT_MAINWINDOW)" << std::endl;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
        return;
    }
    std::cout << "[CameraSelector] Backdrop: Mica unsupported (0x" << std::hex << hr << std::dec << ")" << std::endl;

    hr = SetSystemBackdrop(hwnd, kBackdropTransientWindow);
    if (SUCCEEDED(hr)) {
        std::cout << "[CameraSelector] Backdrop: Acrylic (DWMSBT_TRANSIENTWINDOW)" << std::endl;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
        return;
    }
    std::cout << "[CameraSelector] Backdrop: Acrylic unsupported (0x" << std::hex << hr << std::dec << ")" << std::endl;

    if (TryEnableAeroBlur(hwnd)) {
        std::cout << "[CameraSelector] Backdrop: Aero blur (DwmEnableBlurBehindWindow)" << std::endl;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
        return;
    }

    std::cout << "[CameraSelector] Backdrop: None (unsupported)" << std::endl;
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}

// 计算窗口布局
static void CalculateLayout(DialogState& state) {
    int cameraCount = static_cast<int>(state.cameras.size());

    // 处理没有摄像头的情况
    if (cameraCount == 0) {
        state.windowWidth = CARD_WIDTH + 2 * CARD_SPACING;
        state.windowHeight = TITLE_HEIGHT + BOTTOM_PADDING + CARD_SPACING;
        return;
    }

    int cols = (cameraCount <= 2) ? cameraCount : 2;
    int rows = (cameraCount + cols - 1) / cols;

    state.windowWidth = cols * CARD_WIDTH + (cols + 1) * CARD_SPACING;
    state.windowHeight = TITLE_HEIGHT + rows * CARD_HEIGHT + (rows - 1) * CARD_SPACING + BOTTOM_PADDING + CARD_SPACING;
}

// 获取卡片矩形
static D2D1_RECT_F GetCardRect(const DialogState& state, int index) {
    int cameraCount = static_cast<int>(state.cameras.size());
    if (cameraCount == 0) {
        return D2D1::RectF(0, 0, 0, 0);
    }
    int cols = (cameraCount <= 2) ? cameraCount : 2;

    int col = index % cols;
    int row = index / cols;

    float x = static_cast<float>(CARD_SPACING + col * (CARD_WIDTH + CARD_SPACING));
    float y = static_cast<float>(TITLE_HEIGHT + row * (CARD_HEIGHT + CARD_SPACING));

    return D2D1::RectF(x, y, x + CARD_WIDTH, y + CARD_HEIGHT);
}

static float GetBottomSectionTop(const DialogState& state) {
    return static_cast<float>(state.windowHeight - BOTTOM_PADDING);
}

static D2D1_RECT_F GetCancelButtonRect(const DialogState& state) {
    float totalWidth = BUTTON_WIDTH * 2.0f + BUTTON_GAP;
    float x0 = (state.windowWidth - totalWidth) / 2.0f;
    float y = static_cast<float>(state.windowHeight - BUTTON_BOTTOM_MARGIN - BUTTON_HEIGHT);
    return D2D1::RectF(x0, y, x0 + BUTTON_WIDTH, y + BUTTON_HEIGHT);
}

static D2D1_RECT_F GetOkButtonRect(const DialogState& state) {
    float totalWidth = BUTTON_WIDTH * 2.0f + BUTTON_GAP;
    float x0 = (state.windowWidth - totalWidth) / 2.0f;
    float y = static_cast<float>(state.windowHeight - BUTTON_BOTTOM_MARGIN - BUTTON_HEIGHT);
    float x = x0 + BUTTON_WIDTH + BUTTON_GAP;
    return D2D1::RectF(x, y, x + BUTTON_WIDTH, y + BUTTON_HEIGHT);
}

// 获取复选框矩形
static D2D1_RECT_F GetCheckboxRect(const DialogState& state) {
    float x = static_cast<float>(CARD_SPACING);
    float y = GetBottomSectionTop(state) + CHECKBOX_ROW_TOP;
    return D2D1::RectF(x, y, x + CHECKBOX_SIZE, y + CHECKBOX_SIZE);
}

// 点击测试
static int HitTestCard(const DialogState& state, int x, int y) {
    for (size_t i = 0; i < state.cameras.size(); i++) {
        D2D1_RECT_F rect = GetCardRect(state, static_cast<int>(i));
        if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool HitTestOkButton(const DialogState& state, int x, int y) {
    D2D1_RECT_F rect = GetOkButtonRect(state);
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

static bool HitTestCancelButton(const DialogState& state, int x, int y) {
    D2D1_RECT_F rect = GetCancelButtonRect(state);
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

static bool HitTestCheckbox(const DialogState& state, int x, int y) {
    D2D1_RECT_F rect = GetCheckboxRect(state);
    // 扩大点击区域包含文字
    return x >= rect.left && x <= rect.left + 200 && y >= rect.top - 5 && y <= rect.bottom + 5;
}

// 绘制界面
static void Render(DialogState& state) {
    state.renderer.BeginDraw();
    state.renderer.Clear(state.theme.background);

    // 更新 Ripple
    state.renderer.UpdateRipples(state.dt);

    // 标题
    D2D1_RECT_F titleRect = D2D1::RectF(0, 8, static_cast<float>(state.windowWidth), 42.0f);
    state.renderer.DrawText(L"选择摄像头", titleRect, state.theme.textPrimary, 24.0f,
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // 副标题（更“产品化”一点）
    D2D1_RECT_F subtitleRect = D2D1::RectF(0, 40, static_cast<float>(state.windowWidth), static_cast<float>(TITLE_HEIGHT));
    state.renderer.DrawText(L"请选择要用于手势追踪的摄像头", subtitleRect, state.theme.textSecondary, 13.0f,
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 分隔线
    D2D1_COLOR_F divider = state.theme.cardBorder;
    divider.a *= 0.65f;
    D2D1_RECT_F dividerRect = D2D1::RectF(static_cast<float>(CARD_SPACING),
                                          static_cast<float>(TITLE_HEIGHT - 4),
                                          static_cast<float>(state.windowWidth - CARD_SPACING),
                                          static_cast<float>(TITLE_HEIGHT - 3));
    state.renderer.DrawRoundedRect(dividerRect, 0.0f, divider, nullptr);

    // 摄像头卡片
    for (size_t i = 0; i < state.cameras.size(); i++) {
        int idx = static_cast<int>(i);
        D2D1_RECT_F cardRect = GetCardRect(state, idx);

        // 使用带动画的卡片绘制
        bool isSelected = (idx == state.selectedIndex);
        bool isHovered = (idx == state.hoverIndex);

        if (idx < static_cast<int>(state.cardAnims.size())) {
            state.renderer.DrawAnimatedCard(cardRect, state.cardAnims[idx],
                                            isSelected, isHovered, state.theme, state.dt);
        } else {
            // Fallback
            D2D1_COLOR_F borderColor = state.theme.cardBorder;
            float borderWidth = 1.0f;
            if (isSelected) {
                borderColor = state.theme.cardSelected;
                borderWidth = 2.5f;
            } else if (isHovered) {
                borderColor = state.theme.cardHover;
                borderWidth = 1.5f;
            }
            state.renderer.DrawRoundedRect(cardRect, 12.0f, state.theme.cardBackground, &borderColor, borderWidth);
        }

        // 预览区域
        float previewX = cardRect.left + (CARD_WIDTH - PREVIEW_WIDTH) / 2.0f;
        float previewY = cardRect.top + CARD_PADDING;
        D2D1_RECT_F previewRect = D2D1::RectF(previewX, previewY,
                                               previewX + PREVIEW_WIDTH, previewY + PREVIEW_HEIGHT);

        // 预览背景
        D2D1_COLOR_F previewBg = D2D1::ColorF(0x000000);
        D2D1_COLOR_F previewBorder = (idx == state.selectedIndex) ? state.theme.cardSelected : state.theme.cardBorder;
        previewBorder.a *= (idx == state.selectedIndex) ? 0.90f : 0.55f;
        state.renderer.DrawRoundedRect(previewRect, 8.0f, previewBg, &previewBorder, 1.0f);

        // 获取并绘制摄像头画面
        ID2D1Bitmap* newBitmap = state.previewManager.GetBitmap(idx, state.renderer);
        if (newBitmap) {
            // 释放旧位图
            if (state.cachedBitmaps.count(idx) && state.cachedBitmaps[idx]) {
                state.cachedBitmaps[idx]->Release();
            }
            state.cachedBitmaps[idx] = newBitmap;
        }

        if (state.cachedBitmaps.count(idx) && state.cachedBitmaps[idx]) {
            state.renderer.DrawBitmap(state.cachedBitmaps[idx], previewRect);
        } else {
            // 没有帧时给出占位提示，避免“黑框=坏了”的错觉
            state.renderer.DrawText(L"正在启动预览…", previewRect, state.theme.textSecondary, 13.0f,
                                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // 预览角标：摄像头序号
        {
            float chipW = 72.0f;
            float chipH = 22.0f;
            D2D1_RECT_F chipRect = D2D1::RectF(previewRect.left + 10.0f, previewRect.top + 10.0f,
                                               previewRect.left + 10.0f + chipW, previewRect.top + 10.0f + chipH);
            D2D1_COLOR_F chipBg = state.theme.cardBackground;
            chipBg.a = (std::max)(chipBg.a, 0.90f);
            state.renderer.DrawRoundedRect(chipRect, 11.0f, chipBg, &state.theme.cardBorder, 1.0f);

            std::wstring chipText = L"摄像头 ";
            chipText += std::to_wstring(idx + 1);
            state.renderer.DrawText(chipText, chipRect, state.theme.textPrimary, 12.0f,
                                    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }

        // 选中指示器 (radio button) - 与文字在同一水平线
        float radioX = cardRect.left + 20;
        float radioY = cardRect.bottom - 22;
        float radioRadius = 10.0f;  // 恢复原尺寸

        if (idx == state.selectedIndex) {
            // 选中：填充圆
            state.renderer.DrawCircle(radioX, radioY, radioRadius, state.theme.accent, nullptr);
            state.renderer.DrawCircle(radioX, radioY, 4.0f, D2D1::ColorF(D2D1::ColorF::White), nullptr);
        } else {
            // 未选中：空心圆
            D2D1_COLOR_F transparent = D2D1::ColorF(0, 0, 0, 0);
            state.renderer.DrawCircle(radioX, radioY, radioRadius, transparent, &state.theme.cardBorder, 2.0f);
        }

        // 摄像头名称 - 与 radio 对齐 + 更清晰的层级（字号稍大一点）
        D2D1_RECT_F nameRect = D2D1::RectF(cardRect.left + 40, cardRect.bottom - 36,
                                           cardRect.right - 12, cardRect.bottom - 8);
        state.renderer.DrawText(state.cameras[i].name, nameRect, state.theme.textPrimary, 14.5f,
                                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    // 取消 / 确定按钮（带动画）
    D2D1_RECT_F cancelRect = GetCancelButtonRect(state);
    state.renderer.DrawAnimatedButton(cancelRect, L"取消", state.cancelButtonAnim,
                                      state.cancelHover, state.cancelPressed,
                                      state.theme, state.dt, false);

    D2D1_RECT_F okRect = GetOkButtonRect(state);
    state.renderer.DrawAnimatedButton(okRect, L"确定", state.okButtonAnim,
                                      state.okHover, state.okPressed,
                                      state.theme, state.dt, true);

    // 复选框（带动画）
    D2D1_RECT_F checkboxRect = GetCheckboxRect(state);
    state.renderer.DrawAnimatedCheckbox(checkboxRect, state.checkboxAnim,
                                         state.rememberChoice, state.checkboxHover,
                                         state.checkboxPressed, state.theme, state.dt);

    // 复选框文字
    D2D1_RECT_F checkboxTextRect = D2D1::RectF(checkboxRect.right + 8, checkboxRect.top - 2,
                                                checkboxRect.right + 200, checkboxRect.bottom + 2);
    state.renderer.DrawText(L"记住我的选择", checkboxTextRect, state.theme.textSecondary, 13.0f,
                            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Ripple：只画一次，放在最顶层。之前那种“每张卡片都画一遍所有 ripple”属于纯粹的愚蠢。
    state.renderer.DrawRipples();

    state.renderer.EndDraw();
}

// 窗口过程
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            if (state) {
                Render(*state);
                ValidateRect(hwnd, nullptr);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (state) {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);

                int newHover = HitTestCard(*state, x, y);
                bool newOkHover = HitTestOkButton(*state, x, y);
                bool newCancelHover = HitTestCancelButton(*state, x, y);
                bool newCheckboxHover = HitTestCheckbox(*state, x, y);

                if (newHover != state->hoverIndex || newOkHover != state->okHover || newCancelHover != state->cancelHover ||
                    newCheckboxHover != state->checkboxHover) {
                    state->hoverIndex = newHover;
                    state->okHover = newOkHover;
                    state->cancelHover = newCancelHover;
                    state->checkboxHover = newCheckboxHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                // 设置光标
                if (newHover >= 0 || newOkHover || newCancelHover || newCheckboxHover) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (state) {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);

                int clickedCard = HitTestCard(*state, x, y);
                if (clickedCard >= 0) {
                    // 触发卡片 Ripple
                    D2D1_RECT_F cardRect = GetCardRect(*state, clickedCard);
                    D2D1_COLOR_F cardInk = state->isDarkMode ? state->theme.textPrimary : state->theme.accent;
                    state->renderer.TriggerRipple(static_cast<float>(x), static_cast<float>(y), cardRect, 12.0f, cardInk);

                    if (clickedCard != state->selectedIndex) {
                        state->selectedIndex = clickedCard;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (HitTestCheckbox(*state, x, y)) {
                    state->checkboxPressed = true;
                    // 触发复选框 Ripple
                    D2D1_RECT_F checkboxRect = GetCheckboxRect(*state);
                    D2D1_COLOR_F checkboxInk = state->rememberChoice
                                                   ? D2D1::ColorF(D2D1::ColorF::White)
                                                   : (state->isDarkMode ? state->theme.textPrimary : state->theme.accent);
                    state->renderer.TriggerRipple(static_cast<float>(x), static_cast<float>(y), checkboxRect, 3.0f,
                                                  checkboxInk);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (HitTestOkButton(*state, x, y)) {
                    state->okPressed = true;
                    D2D1_RECT_F buttonRect = GetOkButtonRect(*state);
                    state->renderer.TriggerRipple(static_cast<float>(x), static_cast<float>(y), buttonRect, 6.0f,
                                                  state->theme.buttonText);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (HitTestCancelButton(*state, x, y)) {
                    state->cancelPressed = true;
                    D2D1_RECT_F buttonRect = GetCancelButtonRect(*state);
                    state->renderer.TriggerRipple(static_cast<float>(x), static_cast<float>(y), buttonRect, 6.0f,
                                                  state->theme.textPrimary);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (state) {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);

                // 复选框点击
                if (state->checkboxPressed && HitTestCheckbox(*state, x, y)) {
                    state->rememberChoice = !state->rememberChoice;
                }
                state->checkboxPressed = false;

                // 确定按钮点击
                if (state->okPressed && HitTestOkButton(*state, x, y)) {
                    state->confirmed = true;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
                state->okPressed = false;

                // 取消按钮点击
                if (state->cancelPressed && HitTestCancelButton(*state, x, y)) {
                    state->confirmed = false;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
                state->cancelPressed = false;

                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (state) {
                switch (wParam) {
                    case VK_LEFT:
                    case VK_UP:
                        if (state->selectedIndex > 0) {
                            state->selectedIndex--;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        break;
                    case VK_RIGHT:
                    case VK_DOWN:
                        if (state->selectedIndex < static_cast<int>(state->cameras.size()) - 1) {
                            state->selectedIndex++;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        break;
                    case VK_RETURN:
                    case VK_SPACE:
                        state->confirmed = true;
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        break;
                    case VK_ESCAPE:
                        state->confirmed = false;
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        break;
                    case 'R':
                        state->rememberChoice = !state->rememberChoice;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                }
            }
            return 0;
        }

        case WM_TIMER: {
            if (state && wParam == 1) {
                // 计算 dt
                LARGE_INTEGER currentTime;
                QueryPerformanceCounter(&currentTime);
                if (state->lastTime.QuadPart > 0 && state->frequency.QuadPart > 0) {
                    state->dt = static_cast<float>(currentTime.QuadPart - state->lastTime.QuadPart)
                              / static_cast<float>(state->frequency.QuadPart);
                    // 限制 dt 防止异常
                    if (state->dt > 0.1f) state->dt = 0.1f;
                    if (state->dt < 0.0f) state->dt = 0.001f;
                } else {
                    state->dt = 0.033f; // ~30fps fallback
                }
                state->lastTime = currentTime;

                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 保存选择到注册表
static void SaveCameraChoice(int index) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD value = static_cast<DWORD>(index);
        RegSetValueExW(hKey, REGISTRY_VALUE, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
    }
}

static void SaveSkipCameraSelectorDialog(bool skip) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD value = skip ? 1u : 0u;
        RegSetValueExW(hKey, REGISTRY_VALUE_SKIP, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
    }
}

static bool GetSkipCameraSelectorDialog() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, REGISTRY_VALUE_SKIP, nullptr, &type,
                             reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD) {
            RegCloseKey(hKey);
            return value != 0;
        }
        RegCloseKey(hKey);
    }
    return false;
}

// 公开 API 实现

int GetSavedCameraChoice() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, REGISTRY_VALUE, nullptr, &type,
                             reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD) {
            RegCloseKey(hKey);
            return static_cast<int>(value);
        }
        RegCloseKey(hKey);
    }
    return -1;
}

void ClearSavedCameraChoice() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, REGISTRY_VALUE);
        RegDeleteValueW(hKey, REGISTRY_VALUE_SKIP);
        RegCloseKey(hKey);
    }
}

int ShowCameraSelectorDialog(HWND parentHwnd, HINSTANCE hInstance, bool forceShow) {
    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);

    // 枚举摄像头
    std::vector<CameraInfo> cameras = EnumerateCameras();

    if (cameras.empty() && !forceShow) {
        std::cerr << "[CameraSelector] No cameras found" << std::endl;
        if (comInitialized) CoUninitialize();
        return -1;
    }

    if (cameras.size() == 1 && !forceShow) {
        std::cout << "[CameraSelector] Only one camera, auto-selecting" << std::endl;
        if (comInitialized) CoUninitialize();
        return 0;
    }

    // 检查是否有保存的选择
    int savedChoice = GetSavedCameraChoice();
    bool savedChoiceValid = savedChoice >= 0 && savedChoice < static_cast<int>(cameras.size());
    int  fallbackChoice = savedChoiceValid ? savedChoice : 0;

    // 如果用户勾选"记住我的选择"，且保存的索引仍有效，则不弹窗，直接返回
    if (!forceShow && savedChoiceValid && GetSkipCameraSelectorDialog()) {
        std::cout << "[CameraSelector] Skipping dialog (remember choice): " << savedChoice << std::endl;
        if (comInitialized) CoUninitialize();
        return savedChoice;
    }

    // 初始化对话框状态
    DialogState state;
    state.cameras = std::move(cameras);
    state.isDarkMode = IsSystemDarkMode();
    state.theme = state.isDarkMode ? GetDarkTheme() : GetLightTheme();
    state.selectedIndex = fallbackChoice;
    state.rememberChoice = GetSkipCameraSelectorDialog();

    // 初始化动画状态
    state.cardAnims.resize(state.cameras.size());
    QueryPerformanceFrequency(&state.frequency);
    QueryPerformanceCounter(&state.lastTime);

    CalculateLayout(state);

    // 注册窗口类
    if (!hInstance) {
        hInstance = GetModuleHandleW(nullptr);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hbrBackground = nullptr;  // 我们自己绘制背景

    RegisterClassExW(&wc);

    // 计算窗口位置 (居中)
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowX = (screenWidth - state.windowWidth) / 2;
    int windowY = (screenHeight - state.windowHeight) / 2;

    // 调整窗口大小以包含边框
    RECT rect = { 0, 0, state.windowWidth, state.windowHeight };
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION, FALSE, 0);
    int adjustedWidth = rect.right - rect.left;
    int adjustedHeight = rect.bottom - rect.top;

    // 创建窗口 (作为父窗口的 owned window，父窗口关闭时自动关闭)
    state.hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        L"选择摄像头",
        WS_OVERLAPPED | WS_CAPTION,  // 移除 WS_SYSMENU 禁用关闭按钮
        windowX, windowY, adjustedWidth, adjustedHeight,
        parentHwnd,  // 设置父窗口，使此窗口成为 owned window
        nullptr, hInstance, &state
    );

    if (!state.hwnd) {
        std::cerr << "[CameraSelector] Failed to create window" << std::endl;
        if (comInitialized) CoUninitialize();
        return -1;
    }

    // 应用现代风格
    ApplyBackdropStyle(state.hwnd, state.isDarkMode);

    // 初始化 D2D
    if (!state.renderer.Initialize(state.hwnd)) {
        std::cerr << "[CameraSelector] Failed to initialize D2D" << std::endl;
        DestroyWindow(state.hwnd);
        if (comInitialized) CoUninitialize();
        return -1;
    }

    // 启动摄像头预览
    std::vector<int> indices;
    for (const auto& cam : state.cameras) {
        indices.push_back(cam.index);
    }

    if (!state.previewManager.Initialize(indices, 640, 480, 30)) {
        std::cerr << "[CameraSelector] Failed to initialize camera previews" << std::endl;
    }

    // 设置刷新定时器 (30fps)
    SetTimer(state.hwnd, 1, 33, nullptr);

    // 显示窗口
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 清理
    state.previewManager.StopAll();

    for (auto& pair : state.cachedBitmaps) {
        if (pair.second) {
            pair.second->Release();
        }
    }

    state.renderer.Release();
    UnregisterClassW(WINDOW_CLASS, hInstance);

    if (comInitialized) CoUninitialize();

    // 返回结果
    if (state.confirmed) {
        // “上一次选中的摄像头”：总是保存，供下次默认选中/取消回退使用
        SaveCameraChoice(state.selectedIndex);
        // “记住我的选择”：控制下次是否跳过弹窗
        SaveSkipCameraSelectorDialog(state.rememberChoice);
        std::cout << "[CameraSelector] User selected camera " << state.selectedIndex
                  << (state.rememberChoice ? " (remember)" : "") << std::endl;
        return state.selectedIndex;
    }

    std::cout << "[CameraSelector] User cancelled, using previous camera " << fallbackChoice << std::endl;
    return fallbackChoice;
}

} // namespace CameraSelector

#endif // _WIN32
