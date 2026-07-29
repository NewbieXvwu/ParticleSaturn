#include "Win32WindowManager.h"

#include <algorithm>
#include <dwmapi.h>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")

namespace ParticleSaturn::Win32WindowManager {

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38

enum {
    DWMSBT_AUTO_CUSTOM            = 0,
    DWMSBT_NONE_CUSTOM            = 1,
    DWMSBT_MAINWINDOW_CUSTOM      = 2,
    DWMSBT_TRANSIENTWINDOW_CUSTOM = 3,
    DWMSBT_TABBEDWINDOW_CUSTOM    = 4
};
#endif

bool IsDwmCompositionEnabled() {
    BOOL    enabled = FALSE;
    HRESULT hr      = DwmIsCompositionEnabled(&enabled);
    return SUCCEEDED(hr) && enabled;
}

static void DisableAeroBlur(HWND hwnd) {
    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = FALSE;
    (void)DwmEnableBlurBehindWindow(hwnd, &bb);
}

static bool EnableAeroBlur(HWND hwnd) {
    if (!IsDwmCompositionEnabled()) {
        return false;
    }

    MARGINS margins = {-1, -1, -1, -1};
    (void)DwmExtendFrameIntoClientArea(hwnd, &margins);

    DWM_BLURBEHIND bb{};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    return SUCCEEDED(DwmEnableBlurBehindWindow(hwnd, &bb));
}

static bool IsColorDark(COLORREF c) {
    const double r         = static_cast<double>(GetRValue(c)) / 255.0;
    const double g         = static_cast<double>(GetGValue(c)) / 255.0;
    const double b         = static_cast<double>(GetBValue(c)) / 255.0;
    const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return luminance < 0.5;
}

bool IsSystemDarkMode() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size  = sizeof(value);
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size) ==
            ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return value == 0;
        }
        RegCloseKey(hKey);
    }

    // Win7/Legacy fallback：从系统窗口底色推断
    return IsColorDark(GetSysColor(COLOR_WINDOW));
}

void SetTitleBarDarkMode(HWND hwnd, bool dark) {
    if (hwnd == nullptr) {
        return;
    }
    BOOL    useDarkMode = dark ? TRUE : FALSE;
    HRESULT hr          = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    if (FAILED(hr)) {
        // 兼容旧版本
        (void)DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
}

const char* BackdropName(int mode) {
    switch (mode) {
    case 3:
        return "Mica";
    case 2:
        return "Acrylic";
    case 1:
        return "Aero";
    case 0:
    default:
        return "Solid";
    }
}

void DetectAvailableBackdrops(HWND hwnd, AppState& state) {
    state.backdrop.availableBackdrops.clear();

    if (hwnd == nullptr) {
        state.backdrop.availableBackdrops.push_back(0);
        state.backdrop.backdropIndex  = 0;
        state.backdrop.useTransparent = false;
        return;
    }

    const bool aeroSupported    = IsDwmCompositionEnabled();
    bool       acrylicSupported = false;
    bool       micaSupported    = false;

    // 为测试先扩展 frame
    MARGINS margins = {-1, -1, -1, -1};
    (void)DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Test Acrylic (DWMSBT_TRANSIENTWINDOW = 3)
    {
        int     backdropType = DWMSBT_TRANSIENTWINDOW_CUSTOM;
        HRESULT hr       = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        acrylicSupported = SUCCEEDED(hr);
    }

    // Test Mica (DWMSBT_MAINWINDOW = 2)
    {
        int     backdropType = DWMSBT_MAINWINDOW_CUSTOM;
        HRESULT hr    = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        micaSupported = SUCCEEDED(hr);
    }

    // Reset
    {
        int resetType = DWMSBT_NONE_CUSTOM;
        (void)DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &resetType, sizeof(resetType));
        margins = {0, 0, 0, 0};
        (void)DwmExtendFrameIntoClientArea(hwnd, &margins);
        DisableAeroBlur(hwnd);
    }

    // Cycle order: Mica > Acrylic > Solid > Aero (supported subset)
    if (micaSupported) {
        state.backdrop.availableBackdrops.push_back(3);
    }
    if (acrylicSupported) {
        state.backdrop.availableBackdrops.push_back(2);
    }
    state.backdrop.availableBackdrops.push_back(0);
    if (aeroSupported) {
        state.backdrop.availableBackdrops.push_back(1);
    }

    if (state.backdrop.backdropIndex < 0 ||
        state.backdrop.backdropIndex >= static_cast<int>(state.backdrop.availableBackdrops.size())) {
        state.backdrop.backdropIndex = 0;
    }
}

void SetBackdropMode(HWND hwnd, int mode, AppState& state) {
    if (hwnd == nullptr) {
        state.backdrop.useTransparent = false;
        return;
    }

    std::cout << "[DWM] SetBackdropMode: mode=" << mode << " (" << BackdropName(mode) << ")" << std::endl;

    DisableAeroBlur(hwnd);

    // Reset system backdrop
    {
        int     resetType = DWMSBT_NONE_CUSTOM;
        HRESULT hr        = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &resetType, sizeof(resetType));
        std::cout << "[DWM] Reset backdrop: hr=0x" << std::hex << hr << std::dec << std::endl;
    }

    if (mode == 0) {
        // Solid - 不透明
        MARGINS margins = {0, 0, 0, 0};
        (void)DwmExtendFrameIntoClientArea(hwnd, &margins);
        state.backdrop.useTransparent = false;
        std::cout << "[DWM] Solid mode: useTransparent=false" << std::endl;
    } else if (mode == 1) {
        // Aero - 传统毛玻璃
        bool ok                       = EnableAeroBlur(hwnd);
        state.backdrop.useTransparent = true;
        std::cout << "[DWM] Aero blur: enabled=" << (ok ? "true" : "false") << std::endl;
    } else {
        // Acrylic (mode=2) 或 Mica (mode=3)
        MARGINS margins   = {-1, -1, -1, -1};
        HRESULT hrMargins = DwmExtendFrameIntoClientArea(hwnd, &margins);

        // Acrylic: TRANSIENT(3), Mica: MAINWINDOW(2)
        const int backdropType = (mode == 2) ? DWMSBT_TRANSIENTWINDOW_CUSTOM : DWMSBT_MAINWINDOW_CUSTOM;
        HRESULT   hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        state.backdrop.useTransparent = true;

        std::cout << "[DWM] " << BackdropName(mode) << ": backdropType=" << backdropType << ", hr=0x" << std::hex << hr
                  << ", hrMargins=0x" << hrMargins << std::dec << std::endl;
    }

    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    // 强制 DWM 立即刷新，避免“关闭再开启后无效果/延迟生效”的观感。
    (void)DwmFlush();
}

} // namespace ParticleSaturn::Win32WindowManager
