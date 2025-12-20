#pragma once
// 窗口管理器 - Windows DWM 效果、全屏、主题管理

#include "AppState.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <imm.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")

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

#define DWMSBT_TABBEDWINDOW DWMSBT_TABBEDWINDOW_CUSTOM
#endif

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <GLFW/glfw3.h>

#include <iostream>

namespace WindowManager {

#ifdef _WIN32
// 检测系统是否使用深色模式
inline bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size  = sizeof(value);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
        return value == 0;
    }
    return true;
}

// 设置标题栏深色/浅色模式
inline void SetTitleBarDarkMode(HWND hwnd, bool dark) {
    BOOL    useDarkMode = dark ? TRUE : FALSE;
    HRESULT hr          = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
}

// 检测可用的背景效果
inline void DetectAvailableBackdrops(HWND hwnd, AppState& state) {
    state.backdrop.availableBackdrops.clear();
    state.backdrop.availableBackdrops.push_back(0); // Solid black always available

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Test Acrylic (DWMSBT_TRANSIENTWINDOW = 3)
    int     backdropType = 3;
    HRESULT hr           = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    if (SUCCEEDED(hr)) {
        state.backdrop.availableBackdrops.push_back(1);
        std::cout << "[DWM] Acrylic: Supported" << std::endl;
    } else {
        std::cout << "[DWM] Acrylic: Not supported (0x" << std::hex << hr << std::dec << ")" << std::endl;
    }

    // Test Mica (DWMSBT_MAINWINDOW = 2)
    backdropType = 2;
    hr           = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    if (SUCCEEDED(hr)) {
        state.backdrop.availableBackdrops.push_back(2);
        std::cout << "[DWM] Mica: Supported" << std::endl;
    } else {
        std::cout << "[DWM] Mica: Not supported (0x" << std::hex << hr << std::dec << ")" << std::endl;
    }

    // Reset
    backdropType = 1;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    margins = {0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    std::cout << "[DWM] Available backdrops: ";
    for (int m : state.backdrop.availableBackdrops) {
        const char* names[] = {"Black", "Acrylic", "Mica"};
        std::cout << names[m] << " ";
    }
    std::cout << std::endl;
}

// 设置背景模式：0=纯黑, 1=Acrylic, 2=Mica
inline void SetBackdropMode(HWND hwnd, int mode, AppState& state) {
    int resetType = 1;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &resetType, sizeof(resetType));

    if (mode == 0) {
        MARGINS margins = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        state.backdrop.useTransparent = false;
        std::cout << "[DWM] Backdrop: Solid Black" << std::endl;
    } else {
        MARGINS margins = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        int     backdropType = (mode == 1) ? 3 : 2;
        HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        state.backdrop.useTransparent = true;

        const char* name = (mode == 1) ? "Acrylic" : "Mica";
        std::cout << "[DWM] Backdrop: " << name << " (type=" << backdropType << ") "
                  << (SUCCEEDED(hr) ? "OK" : "FAILED") << std::endl;
    }

    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
}

// 切换全屏模式
inline void ToggleFullscreen(GLFWwindow* window, AppState& state) {
    if (!state.window.isFullscreen) {
        glfwGetWindowPos(window, &state.window.windowedX, &state.window.windowedY);
        glfwGetWindowSize(window, &state.window.windowedW, &state.window.windowedH);

        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode    = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        state.window.isFullscreen = true;
        std::cout << "[Window] Fullscreen: " << mode->width << "x" << mode->height << std::endl;
    } else {
        glfwSetWindowMonitor(window, nullptr, state.window.windowedX, state.window.windowedY, state.window.windowedW,
                             state.window.windowedH, 0);
        state.window.isFullscreen = false;
        std::cout << "[Window] Windowed: " << state.window.windowedW << "x" << state.window.windowedH << std::endl;
    }
}

// 主题变更钩子
static WNDPROC g_originalWndProc = nullptr;
static HWND    g_mainHwnd        = nullptr;

void OnThemeChanged(bool isDark);

inline LRESULT CALLBACK ThemeAwareWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETTINGCHANGE) {
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            bool newDarkMode = IsSystemDarkMode();
            // 通过 GLFW 窗口获取 AppState
            GLFWwindow* glfwWindow = nullptr;
            // 注意：这里我们需要一个机制来获取 GLFWwindow，但 WndProc 中无法直接获取
            // 暂时保持使用回调机制
            OnThemeChanged(newDarkMode);
        }
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
}

inline void InstallThemeChangeHook(HWND hwnd) {
    g_mainHwnd        = hwnd;
    g_originalWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)ThemeAwareWndProc);
    if (g_originalWndProc) {
        std::cout << "[Main] Theme change hook installed" << std::endl;
    }
}
#endif

// 帧缓冲大小变更回调
inline void FramebufferSizeCallback(GLFWwindow* window, int width, int height) {
    if (width > 0 && height > 0) {
        AppState* state = GetAppState(window);
        if (state) {
            state->window.width   = width;
            state->window.height  = height;
            state->window.resized = true;
        }
        glViewport(0, 0, width, height);
    }
}

} // namespace WindowManager
