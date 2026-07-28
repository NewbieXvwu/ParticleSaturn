#include <windows.h>

#include <imm.h> // IME 控制
#include <iostream>
#include <shellscalingapi.h> // GetDpiForWindow (Win10 1607+)
#include <string>

#pragma comment(lib, "imm32.lib")

#include "../AppState.h"
#include "../DebugLog.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "../Settings.h"
#include "DiligentBackend.h"
#include "RenderBackend.h"
#include "Win32WindowManager.h"
#include "md3/MD3.h"
#include "platform/windows/Win32AppHost.h"
#include "platform/windows/Win32InputMapper.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"ParticleSaturn.Diligent";
constexpr wchar_t kWindowTitle[]     = L"Particle Saturn (Diligent)";

// Win32 应用外壳辅助（IsDirectCompositionSupported / GetDpiScaleForWindow /
// ParseBackendFromCmdLine / GetClientSize）已抽出到
// ParticleSaturn::Platform::Windows（Win32AppHost.h，D-015 重启 Phase A）。
using ParticleSaturn::Platform::Windows::GetClientSize;
using ParticleSaturn::Platform::Windows::GetDpiScaleForWindow;
using ParticleSaturn::Platform::Windows::IsDirectCompositionSupported;
using ParticleSaturn::Platform::Windows::ParseBackendFromCmdLine;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* backend = reinterpret_cast<ParticleSaturn::Render::DiligentBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 转发消息给 ImGui
    if (backend != nullptr && backend->HandleWin32Message(hwnd, msg, wParam, lParam)) {
        return 0; // ImGui 已处理此消息
    }

    // 输入/尺寸/DPI/主题消息映射（D-015 重启 Phase A：外壳职责已抽到
    // Platform::Windows::DispatchWindowMessage）。
    if (backend != nullptr) {
        auto* state = backend->GetAppState();
        if (state != nullptr &&
            ParticleSaturn::Platform::Windows::DispatchWindowMessage(hwnd, msg, wParam, lParam, *state, *backend)) {
            return 0;
        }
    }

    switch (msg) {
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
    std::cout << "[Main] Backend: "
              << (requestedBackend == ParticleSaturn::Render::Backend::D3D11    ? "D3D11"
                  : requestedBackend == ParticleSaturn::Render::Backend::Vulkan ? "Vulkan"
                                                                                : "D3D12")
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

    // 从注册表加载窗口位置/大小
    Settings::WindowState savedWindow = Settings::LoadWindowState();
    int                   windowX     = CW_USEDEFAULT;
    int                   windowY     = CW_USEDEFAULT;
    int                   windowW     = 1280;
    int                   windowH     = 720;
    if (savedWindow.valid) {
        windowX = savedWindow.x;
        windowY = savedWindow.y;
        windowW = savedWindow.w;
        windowH = savedWindow.h;
        std::cout << "[Main] Restored window position: " << windowX << "," << windowY << " size: " << windowW << "x"
                  << windowH << std::endl;
    }

    RECT wr{0, 0, windowW, windowH};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    OutputDebugStringW(L"[ParticleSaturn] Creating window\n");
    HWND hwnd = CreateWindowExW(exStyle, kWindowClassName, kWindowTitle, style, windowX, windowY, wr.right - wr.left,
                                wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) {
        OutputDebugStringW(L"[ParticleSaturn] Window creation FAILED\n");
        ErrorHandler::ShowEarlyFatalError(i18n::Get().windowCreateFailed, i18n::Get().detailWindowCreateFailed);
        return -1;
    }
    OutputDebugStringW(L"[ParticleSaturn] Window created OK\n");

    // 禁用输入法或切换到英文模式（防止启动时自动激活中文输入法）
    HIMC hIMC = ImmGetContext(hwnd);
    if (hIMC != nullptr) {
        // 设置为英文模式（关闭中文输入）
        ImmSetConversionStatus(hIMC, IME_CMODE_ALPHANUMERIC, IME_SMODE_NONE);
        ImmReleaseContext(hwnd, hIMC);
    }

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

    // 从注册表加载会话状态（覆盖默认值）
    Settings::LoadSession(appState);

    // 恢复窗口状态
    if (savedWindow.valid) {
        appState.window.windowedX    = savedWindow.x;
        appState.window.windowedY    = savedWindow.y;
        appState.window.windowedW    = savedWindow.w;
        appState.window.windowedH    = savedWindow.h;
        appState.window.isFullscreen = savedWindow.fullscreen;
    }

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
                // 退出前保存会话状态
                // 更新当前窗口位置（仅非全屏模式）
                if (!appState.window.isFullscreen) {
                    RECT wr;
                    if (GetWindowRect(hwnd, &wr)) {
                        appState.window.windowedX = wr.left;
                        appState.window.windowedY = wr.top;
                        appState.window.windowedW = wr.right - wr.left;
                        appState.window.windowedH = wr.bottom - wr.top;
                    }
                }
                Settings::SaveSession(appState, requestedBackend);

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
