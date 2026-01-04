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

// 注册表路径
static const wchar_t* REGISTRY_KEY = L"SOFTWARE\\ParticleSaturn";
static const wchar_t* REGISTRY_VALUE = L"SelectedCamera";

// 窗口类名
static const wchar_t* WINDOW_CLASS = L"CameraSelectorWindow";

// UI 常量
static constexpr int CARD_WIDTH = 340;
static constexpr int CARD_HEIGHT = 300;
static constexpr int CARD_PADDING = 20;
static constexpr int PREVIEW_WIDTH = 320;
static constexpr int PREVIEW_HEIGHT = 240;
static constexpr int CARD_SPACING = 20;
static constexpr int TITLE_HEIGHT = 60;
static constexpr int BUTTON_HEIGHT = 40;
static constexpr int BUTTON_WIDTH = 120;
static constexpr int CHECKBOX_SIZE = 20;
static constexpr int BOTTOM_PADDING = 80;

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
    bool buttonHover = false;
    bool confirmed = false;
    bool isDarkMode = true;

    // 缓存的位图
    std::map<int, ID2D1Bitmap*> cachedBitmaps;

    HWND hwnd = nullptr;
    int windowWidth = 0;
    int windowHeight = 0;
};

// 检测系统深色模式
static bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&value), &size);
        RegCloseKey(hKey);
        return value == 0;
    }
    return true;  // 默认深色
}

// 应用 Windows 11 风格
static void ApplyModernStyle(HWND hwnd, bool darkMode) {
    // 圆角窗口 (Windows 11)
    enum DWM_WINDOW_CORNER_PREFERENCE { DWMWCP_DEFAULT = 0, DWMWCP_DONOTROUND = 1, DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3 };
    DWORD cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

    // 深色模式标题栏
    BOOL useDark = darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &useDark, sizeof(useDark));

    // Mica 背景 (Windows 11 22H2+)
    enum DWM_SYSTEMBACKDROP_TYPE { DWMSBT_AUTO = 0, DWMSBT_NONE = 1, DWMSBT_MAINWINDOW = 2, DWMSBT_TRANSIENTWINDOW = 3, DWMSBT_TABBEDWINDOW = 4 };
    DWORD backdrop = DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

// 计算窗口布局
static void CalculateLayout(DialogState& state) {
    int cameraCount = static_cast<int>(state.cameras.size());
    int cols = (cameraCount <= 2) ? cameraCount : 2;
    int rows = (cameraCount + cols - 1) / cols;

    state.windowWidth = cols * CARD_WIDTH + (cols + 1) * CARD_SPACING;
    state.windowHeight = TITLE_HEIGHT + rows * CARD_HEIGHT + (rows - 1) * CARD_SPACING + BOTTOM_PADDING + CARD_SPACING;
}

// 获取卡片矩形
static D2D1_RECT_F GetCardRect(const DialogState& state, int index) {
    int cameraCount = static_cast<int>(state.cameras.size());
    int cols = (cameraCount <= 2) ? cameraCount : 2;

    int col = index % cols;
    int row = index / cols;

    float x = static_cast<float>(CARD_SPACING + col * (CARD_WIDTH + CARD_SPACING));
    float y = static_cast<float>(TITLE_HEIGHT + row * (CARD_HEIGHT + CARD_SPACING));

    return D2D1::RectF(x, y, x + CARD_WIDTH, y + CARD_HEIGHT);
}

// 获取按钮矩形
static D2D1_RECT_F GetButtonRect(const DialogState& state) {
    float x = (state.windowWidth - BUTTON_WIDTH) / 2.0f;
    float y = static_cast<float>(state.windowHeight - BOTTOM_PADDING + 10);
    return D2D1::RectF(x, y, x + BUTTON_WIDTH, y + BUTTON_HEIGHT);
}

// 获取复选框矩形
static D2D1_RECT_F GetCheckboxRect(const DialogState& state) {
    float x = static_cast<float>(CARD_SPACING);
    float y = static_cast<float>(state.windowHeight - BOTTOM_PADDING + 15);
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

static bool HitTestButton(const DialogState& state, int x, int y) {
    D2D1_RECT_F rect = GetButtonRect(state);
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

    // 标题
    D2D1_RECT_F titleRect = D2D1::RectF(0, 10, static_cast<float>(state.windowWidth), static_cast<float>(TITLE_HEIGHT));
    state.renderer.DrawText(L"选择摄像头", titleRect, state.theme.textPrimary, 24.0f,
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // 摄像头卡片
    for (size_t i = 0; i < state.cameras.size(); i++) {
        int idx = static_cast<int>(i);
        D2D1_RECT_F cardRect = GetCardRect(state, idx);

        // 卡片背景
        D2D1_COLOR_F borderColor = state.theme.cardBorder;
        float borderWidth = 1.0f;

        if (idx == state.selectedIndex) {
            borderColor = state.theme.cardSelected;
            borderWidth = 2.5f;
        } else if (idx == state.hoverIndex) {
            borderColor = state.theme.cardHover;
            borderWidth = 1.5f;
        }

        state.renderer.DrawRoundedRect(cardRect, 12.0f, state.theme.cardBackground, &borderColor, borderWidth);

        // 预览区域
        float previewX = cardRect.left + (CARD_WIDTH - PREVIEW_WIDTH) / 2.0f;
        float previewY = cardRect.top + CARD_PADDING;
        D2D1_RECT_F previewRect = D2D1::RectF(previewX, previewY,
                                               previewX + PREVIEW_WIDTH, previewY + PREVIEW_HEIGHT);

        // 预览背景
        D2D1_COLOR_F previewBg = D2D1::ColorF(0x000000);
        state.renderer.DrawRoundedRect(previewRect, 8.0f, previewBg, nullptr);

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
        }

        // 选中指示器 (radio button) - 与文字在同一水平线
        float radioX = cardRect.left + 20;
        float radioY = cardRect.bottom - 22;  // 往下移
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

        // 摄像头名称 - 调整位置与 radio 对齐
        D2D1_RECT_F nameRect = D2D1::RectF(cardRect.left + 40, cardRect.bottom - 35,
                                            cardRect.right - 10, cardRect.bottom - 8);
        state.renderer.DrawText(state.cameras[i].name, nameRect, state.theme.textPrimary, 14.0f,
                                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 确定按钮
    D2D1_RECT_F buttonRect = GetButtonRect(state);
    D2D1_COLOR_F buttonBg = state.buttonHover ? state.theme.buttonHover : state.theme.buttonBg;
    state.renderer.DrawRoundedRect(buttonRect, 6.0f, buttonBg, nullptr);
    state.renderer.DrawText(L"确定", buttonRect, state.theme.buttonText, 14.0f,
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // 复选框
    D2D1_RECT_F checkboxRect = GetCheckboxRect(state);
    state.renderer.DrawCheckbox(checkboxRect, state.rememberChoice, state.theme);

    // 复选框文字
    D2D1_RECT_F checkboxTextRect = D2D1::RectF(checkboxRect.right + 8, checkboxRect.top - 2,
                                                checkboxRect.right + 200, checkboxRect.bottom + 2);
    state.renderer.DrawText(L"记住我的选择", checkboxTextRect, state.theme.textSecondary, 13.0f,
                            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

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
                bool newButtonHover = HitTestButton(*state, x, y);
                bool newCheckboxHover = HitTestCheckbox(*state, x, y);

                if (newHover != state->hoverIndex || newButtonHover != state->buttonHover ||
                    newCheckboxHover != state->checkboxHover) {
                    state->hoverIndex = newHover;
                    state->buttonHover = newButtonHover;
                    state->checkboxHover = newCheckboxHover;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                // 设置光标
                if (newHover >= 0 || newButtonHover || newCheckboxHover) {
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
                if (clickedCard >= 0 && clickedCard != state->selectedIndex) {
                    state->selectedIndex = clickedCard;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (HitTestCheckbox(*state, x, y)) {
                    state->rememberChoice = !state->rememberChoice;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                if (HitTestButton(*state, x, y)) {
                    state->confirmed = true;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
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
        RegCloseKey(hKey);
    }
}

int ShowCameraSelectorDialog(HINSTANCE hInstance) {
    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitialized = SUCCEEDED(hr);

    // 枚举摄像头
    std::vector<CameraInfo> cameras = EnumerateCameras();

    if (cameras.empty()) {
        std::cerr << "[CameraSelector] No cameras found" << std::endl;
        if (comInitialized) CoUninitialize();
        return -1;
    }

    if (cameras.size() == 1) {
        std::cout << "[CameraSelector] Only one camera, auto-selecting" << std::endl;
        if (comInitialized) CoUninitialize();
        return 0;
    }

    // 检查是否有保存的选择
    int savedChoice = GetSavedCameraChoice();
    if (savedChoice >= 0 && savedChoice < static_cast<int>(cameras.size())) {
        std::cout << "[CameraSelector] Using saved camera choice: " << savedChoice << std::endl;
        if (comInitialized) CoUninitialize();
        return savedChoice;
    }

    // 初始化对话框状态
    DialogState state;
    state.cameras = std::move(cameras);
    state.isDarkMode = IsSystemDarkMode();
    state.theme = state.isDarkMode ? GetDarkTheme() : GetLightTheme();

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

    // 创建窗口 (无关闭按钮，用户必须通过确定按钮或 ESC 退出)
    state.hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        L"选择摄像头",
        WS_OVERLAPPED | WS_CAPTION,  // 移除 WS_SYSMENU 禁用关闭按钮
        windowX, windowY, adjustedWidth, adjustedHeight,
        nullptr, nullptr, hInstance, &state
    );

    if (!state.hwnd) {
        std::cerr << "[CameraSelector] Failed to create window" << std::endl;
        if (comInitialized) CoUninitialize();
        return -1;
    }

    // 应用现代风格
    ApplyModernStyle(state.hwnd, state.isDarkMode);

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
        if (state.rememberChoice) {
            SaveCameraChoice(state.selectedIndex);
        }
        std::cout << "[CameraSelector] User selected camera " << state.selectedIndex << std::endl;
        return state.selectedIndex;
    }

    std::cout << "[CameraSelector] User cancelled" << std::endl;
    return -1;
}

} // namespace CameraSelector

#endif // _WIN32
