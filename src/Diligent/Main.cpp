#include <windows.h>

#include <string>

#include "../AppState.h"
#include "DiligentBackend.h"
#include "RenderBackend.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"ParticleSaturn.Diligent";
constexpr wchar_t kWindowTitle[]     = L"Particle Saturn (Diligent)";

ParticleSaturn::Render::Backend ParseBackendFromCmdLine(const std::wstring& cmdLine) {
    // 支持：
    //   --backend=d3d12
    //   --backend=vulkan
    // 默认：D3D12（Windows 上更常见，且无需 Vulkan SDK 即可运行）。
    if (cmdLine.find(L"--backend=vulkan") != std::wstring::npos ||
        cmdLine.find(L"--backend=vk") != std::wstring::npos) {
        return ParticleSaturn::Render::Backend::Vulkan;
    }
    return ParticleSaturn::Render::Backend::D3D12;
}

ParticleSaturn::Render::SurfaceSize GetClientSize(HWND hwnd) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return {};
    }
    const auto w = static_cast<uint32_t>(rc.right - rc.left);
    const auto h = static_cast<uint32_t>(rc.bottom - rc.top);
    return {w, h};
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* backend = reinterpret_cast<ParticleSaturn::Render::DiligentBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 转发消息给 ImGui
    if (backend != nullptr && backend->HandleWin32Message(hwnd, msg, wParam, lParam)) {
        return 0; // ImGui 已处理此消息
    }

    switch (msg) {
    case WM_KEYDOWN: {
        if (backend != nullptr) {
            auto* state = backend->GetAppState();
            if (state != nullptr) {
                switch (wParam) {
                case VK_F3:
                    // 防抖：只在按下时触发一次
                    if (!state->input.keyF3_pressed) {
                        state->input.keyF3_pressed = true;
                        state->ui.showDebugWindow  = !state->ui.showDebugWindow;
                    }
                    break;
                case VK_F11:
                    if (!state->input.keyF11_pressed) {
                        state->input.keyF11_pressed = true;
                        // 切换全屏
                        state->window.isFullscreen = !state->window.isFullscreen;
                        if (state->window.isFullscreen) {
                            // 保存窗口位置
                            RECT wr;
                            GetWindowRect(hwnd, &wr);
                            state->window.windowedX = wr.left;
                            state->window.windowedY = wr.top;
                            state->window.windowedW = wr.right - wr.left;
                            state->window.windowedH = wr.bottom - wr.top;
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
                            SetWindowPos(hwnd, nullptr, state->window.windowedX, state->window.windowedY,
                                         state->window.windowedW, state->window.windowedH,
                                         SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
                        }
                    }
                    break;
                case 'B':
                    if (!state->input.keyB_pressed) {
                        state->input.keyB_pressed = true;
                        // 循环切换背景
                        state->backdrop.backdropIndex = (state->backdrop.backdropIndex + 1) %
                                                        static_cast<int>(state->backdrop.availableBackdrops.size());
                    }
                    break;
                case VK_ESCAPE:
                    PostQuitMessage(0);
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_KEYUP: {
        if (backend != nullptr) {
            auto* state = backend->GetAppState();
            if (state != nullptr) {
                switch (wParam) {
                case VK_F3:
                    state->input.keyF3_pressed = false;
                    break;
                case VK_F11:
                    state->input.keyF11_pressed = false;
                    break;
                case 'B':
                    state->input.keyB_pressed = false;
                    break;
                }
            }
        }
        return 0;
    }
    case WM_SIZE: {
        if (backend != nullptr && wParam != SIZE_MINIMIZED) {
            const auto w = static_cast<uint32_t>(LOWORD(lParam));
            const auto h = static_cast<uint32_t>(HIWORD(lParam));
            backend->Resize({w, h});
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const std::wstring cmdLine = GetCommandLineW();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;

    RECT wr{0, 0, 1280, 720};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    HWND hwnd = CreateWindowExW(exStyle, kWindowClassName, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) {
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ParticleSaturn::Render::DiligentBackend backend{};
    AppState                                appState{};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&backend));

    const auto surface = GetClientSize(hwnd);
    if (!backend.Init(ParseBackendFromCmdLine(cmdLine), hwnd, surface, &appState)) {
        std::wstring msg = L"初始化 Diligent 失败。\n\n";
        const auto&  err = backend.GetLastError();
        if (!err.empty()) {
            msg += L"原因：";
            msg += err;
            msg += L"\n\n";
        }
        msg += L"请确认 D3D12/Vulkan 环境可用。";
        MessageBoxW(hwnd, msg.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
        return -2;
    }

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                backend.Shutdown();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        backend.RenderFrame();
    }
}
