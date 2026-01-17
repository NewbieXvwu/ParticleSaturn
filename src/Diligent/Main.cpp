#include <windows.h>

#include <shellscalingapi.h> // GetDpiForWindow (Win10 1607+)
#include <iostream>
#include <string>

#include "../AppState.h"
#include "../DebugLog.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "DiligentBackend.h"
#include "RenderBackend.h"
#include "Win32WindowManager.h"
#include "md3/MD3.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"ParticleSaturn.Diligent";
constexpr wchar_t kWindowTitle[]     = L"Particle Saturn (Diligent)";

// 检测 Windows 版本是否支持 DirectComposition 透明窗口 + Mica
// Win11 22H2+ (Build 22621+) 支持 DWMWA_SYSTEMBACKDROP_TYPE
// Win10 1803+ (Build 17134+) 支持 WS_EX_NOREDIRECTIONBITMAP 但不支持 Mica
bool IsDirectCompositionSupported() {
    // 使用 RtlGetVersion 获取真实版本（不受兼容性 shim 影响）
    using RtlGetVersionPtr = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }

    auto RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (RtlGetVersion == nullptr) {
        return false;
    }

    RTL_OSVERSIONINFOW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (RtlGetVersion(&osvi) != 0) {
        return false;
    }

    // Win10 1803+ (Build 17134+) 支持 WS_EX_NOREDIRECTIONBITMAP + DirectComposition
    // Win11 (Build 22000+) 支持 Mica
    // 保守起见，只在 Win10 1809+ (Build 17763+) 启用
    const bool isWin10_1809OrLater = (osvi.dwMajorVersion > 10) ||
                                     (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 17763);

    if (isWin10_1809OrLater) {
        std::cout << "[Main] Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion
                  << " Build " << osvi.dwBuildNumber << " - DirectComposition supported" << std::endl;
        return true;
    }

    std::cout << "[Main] Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion
              << " Build " << osvi.dwBuildNumber << " - DirectComposition NOT supported" << std::endl;
    return false;
}

// 获取窗口的 DPI 缩放因子（相对于 96 DPI）
float GetDpiScaleForWindow(HWND hwnd) {
    // 优先使用 GetDpiForWindow (Win10 1607+)
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) {
        // 回退到系统 DPI
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
            ReleaseDC(hwnd, hdc);
        }
    }
    if (dpi == 0) {
        dpi = 96; // 默认 96 DPI
    }
    return static_cast<float>(dpi) / 96.0f;
}

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
    case WM_SETTINGCHANGE: {
        // 系统主题切换（Win10/11）：更新标题栏暗色模式 + 应用内 dark mode
        if (lParam != 0 && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
            if (backend != nullptr) {
                auto* state = backend->GetAppState();
                if (state != nullptr) {
                    const bool dark      = ParticleSaturn::Win32WindowManager::IsSystemDarkMode();
                    state->ui.isDarkMode = dark;
                    ParticleSaturn::Win32WindowManager::SetTitleBarDarkMode(hwnd, dark);
                    if (backend->IsInitialized()) {
                        MD3::ApplyImGuiStyle();
                    }
                }
            }
        }
        return 0;
    }
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
                        if (!state->backdrop.availableBackdrops.empty()) {
                            state->backdrop.backdropIndex = (state->backdrop.backdropIndex + 1) %
                                                            static_cast<int>(state->backdrop.availableBackdrops.size());
                            const int mode = state->backdrop.availableBackdrops[state->backdrop.backdropIndex];
                            ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd, mode, *state);
                        }
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
    case WM_DPICHANGED: {
        // DPI 变化时（如拖动窗口到不同 DPI 显示器）
        if (backend != nullptr) {
            const float newDpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            auto*       state       = backend->GetAppState();
            if (state != nullptr) {
                state->ui.dpiScale = newDpiScale;
            }
            // 更新 MD3/ImGui 的 DPI 缩放
            MD3::SetDpiScale(newDpiScale);

            // 按系统建议调整窗口大小
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
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
    // 最早的日志：使用 OutputDebugString，在任何 C++ 初始化之前
    OutputDebugStringW(L"[ParticleSaturn] wWinMain entry\n");

    // 将 stdout/stderr 重定向到 DebugLog（用于 ImGui 日志面板）
    // 说明：DebugStreamBuf 会同时把内容写回原始 streambuf，因此 DebugView/VS Output 里仍能看到。
    static DebugStreamBuf s_debugOut(std::cout.rdbuf(), LogLevel::Info);
    static DebugStreamBuf s_debugErr(std::cerr.rdbuf(), LogLevel::Error);
    std::cout.rdbuf(&s_debugOut);
    std::cerr.rdbuf(&s_debugErr);

    OutputDebugStringW(L"[ParticleSaturn] DebugStreamBuf initialized\n");

    // 统一错误处理/崩溃捕获：尽早初始化异常处理器
    ErrorHandler::Init();
    ErrorHandler::SetStage(ErrorHandler::AppStage::STARTUP);

    OutputDebugStringW(L"[ParticleSaturn] ErrorHandler initialized\n");

    const std::wstring cmdLine = GetCommandLineW();
    std::cout << "[Main] Particle Saturn " << i18n::GetVersion() << " starting..." << std::endl;
    const auto requestedBackend = ParseBackendFromCmdLine(cmdLine);
    std::cout << "[Main] Backend: " << (requestedBackend == ParticleSaturn::Render::Backend::Vulkan ? "Vulkan" : "D3D12")
              << std::endl;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    ErrorHandler::SetStage(ErrorHandler::AppStage::WINDOW_INIT);
    OutputDebugStringW(L"[ParticleSaturn] Registering window class\n");
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW;

    // 检测是否支持 DirectComposition 透明窗口
    // WS_EX_NOREDIRECTIONBITMAP: Win10 1809+ 支持，但 Mica 需要 Win11 22H2+
    // 在不支持的系统上使用普通窗口（无透明效果）
    const bool  useDComp = IsDirectCompositionSupported();
    const DWORD exStyle  = useDComp ? WS_EX_NOREDIRECTIONBITMAP : 0;

    if (!useDComp) {
        std::cout << "[Main] DirectComposition not supported, transparent effects disabled" << std::endl;
    }

    RECT wr{0, 0, 1280, 720};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    OutputDebugStringW(L"[ParticleSaturn] Creating window\n");
    HWND hwnd = CreateWindowExW(exStyle, kWindowClassName, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) {
        OutputDebugStringW(L"[ParticleSaturn] Window creation FAILED\n");
        ErrorHandler::ShowEarlyFatalError(i18n::Get().windowCreateFailed, i18n::Get().detailWindowCreateFailed);
        return -1;
    }
    OutputDebugStringW(L"[ParticleSaturn] Window created OK\n");

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    OutputDebugStringW(L"[ParticleSaturn] Creating DiligentBackend\n");
    ParticleSaturn::Render::DiligentBackend backend{};
    AppState                                appState{};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&backend));

    // 标记是否支持透明效果（影响后续 SwapChain 选择和 UI 显示）
    appState.backdrop.transparentSupported = useDComp;

    // 初始化 AppState 基本值（对齐 OpenGL：默认粒子数为 MAX=1200000）
    appState.InitDefaults(1200000);

    // 系统主题/窗口效果初始化
    {
        const bool dark        = ParticleSaturn::Win32WindowManager::IsSystemDarkMode();
        appState.ui.isDarkMode = dark;
        ParticleSaturn::Win32WindowManager::SetTitleBarDarkMode(hwnd, dark);
        std::cout << "[DWM] System theme: " << (dark ? "Dark" : "Light") << std::endl;

        ParticleSaturn::Win32WindowManager::DetectAvailableBackdrops(hwnd, appState);
        if (!appState.backdrop.availableBackdrops.empty()) {
            const int mode = appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex];
            ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd, mode, appState);
        }

        // 初始化 DPI 缩放
        appState.ui.dpiScale = GetDpiScaleForWindow(hwnd);
        std::cout << "[Main] DPI scale: " << appState.ui.dpiScale << std::endl;
    }

    OutputDebugStringW(L"[ParticleSaturn] Calling backend.Init()\n");
    const auto surface = GetClientSize(hwnd);
    if (!backend.Init(requestedBackend, hwnd, surface, &appState)) {
        OutputDebugStringW(L"[ParticleSaturn] backend.Init() FAILED\n");
        // ShowEarlyFatalError 需要 UTF-8 字符串
        auto WideToUtf8 = [](const std::wstring& w) -> std::string {
            if (w.empty()) {
                return {};
            }
            int n =
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0) {
                return {};
            }
            std::string out(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        };

        std::string details = "DiligentBackend::Init() failed\n\n";
        details += WideToUtf8(backend.GetLastError());
        details += "\n\nPlease confirm D3D12/Vulkan runtime is available.";

        ErrorHandler::ShowEarlyFatalError(i18n::Get().unexpectedError, details.c_str());
        return -2;
    }
    ErrorHandler::SetStage(ErrorHandler::AppStage::RENDER_LOOP);
    std::cout << "[Main] Diligent backend initialized" << std::endl;

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                backend.Shutdown();
                ErrorHandler::SetStage(ErrorHandler::AppStage::SHUTDOWN);
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        backend.RenderFrame();
    }
}
