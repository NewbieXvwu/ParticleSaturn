#include "Win32InputMapper.h"

#include "AppState.h"
#include "DiligentBackend.h"
#include "Win32WindowManager.h"
#include "imgui.h"
#include "md3/MD3.h"

namespace ParticleSaturn::Platform::Windows {

bool DispatchWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, AppState& state,
                           Render::DiligentBackend& backend) {
    switch (msg) {
    case WM_SETTINGCHANGE: {
        // 系统主题切换（Win10/11）：更新标题栏暗色模式 + 应用内 dark mode
        if (lParam != 0 && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            const bool dark      = ParticleSaturn::Win32WindowManager::IsSystemDarkMode();
            state.ui.isDarkMode  = dark;
            ParticleSaturn::Win32WindowManager::SetTitleBarDarkMode(hwnd, dark);
            if (backend.IsInitialized()) {
                MD3::ApplyImGuiStyle();
            }
        }
        return true;
    }
    case WM_KEYDOWN: {
        switch (wParam) {
        case VK_F3:
            // 防抖：只在按下时触发一次
            if (!state.input.keyF3_pressed) {
                state.input.keyF3_pressed = true;
                state.ui.showDebugWindow  = !state.ui.showDebugWindow;
            }
            break;
        case VK_F11:
            if (!state.input.keyF11_pressed) {
                state.input.keyF11_pressed = true;
                // 切换全屏
                state.window.isFullscreen = !state.window.isFullscreen;
                if (state.window.isFullscreen) {
                    // 保存窗口位置
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    state.window.windowedX = wr.left;
                    state.window.windowedY = wr.top;
                    state.window.windowedW = wr.right - wr.left;
                    state.window.windowedH = wr.bottom - wr.top;
                    // 进入全屏
                    HMONITOR    hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi   = {sizeof(mi)};
                    GetMonitorInfoW(hMon, &mi);
                    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
                    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                } else {
                    // 退出全屏
                    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                    SetWindowPos(hwnd, nullptr, state.window.windowedX, state.window.windowedY,
                                 state.window.windowedW, state.window.windowedH,
                                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
                }
            }
            break;
        case 'B':
            if (!state.input.keyB_pressed) {
                state.input.keyB_pressed = true;
                // B 键切换模糊效果开关（只有开/关两种状态）
                state.ui.enableBlur = !state.ui.enableBlur;
            }
            break;
        case VK_ESCAPE:
            PostQuitMessage(0);
            return true;
        }
        return true;
    }
    case WM_KEYUP: {
        switch (wParam) {
        case VK_F3:
            state.input.keyF3_pressed = false;
            break;
        case VK_F11:
            state.input.keyF11_pressed = false;
            break;
        case 'B':
            state.input.keyB_pressed = false;
            break;
        }
        return true;
    }
    case WM_SIZE: {
        if (wParam != SIZE_MINIMIZED) {
            const auto w = static_cast<uint32_t>(LOWORD(lParam));
            const auto h = static_cast<uint32_t>(HIWORD(lParam));
            backend.RequestResize({w, h});
        }
        return true;
    }
    case WM_DPICHANGED: {
        // DPI 变化时（如拖动窗口到不同 DPI 显示器）
        const float newDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
        state.ui.dpiScale        = newDpiScale;
        // 更新 MD3/ImGui 的 DPI 缩放
        MD3::SetDpiScale(newDpiScale);

        // 按系统建议调整窗口大小
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }
    default:
        return false;
    }
}

} // namespace ParticleSaturn::Platform::Windows
