#include <windows.h>

#include <algorithm>
#include <chrono>
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
#include "platform/windows/Win32WindowManager.h"
#include "services/hand_tracking/windows/HandTrackerController.h"
#include "app/AppController.h"
#include "app/FpsMeter.h"
#include "app/FrameCoordinator.h"
#include "app/RenderSeam.h"
#include "md3/MD3.h"
#include "md3/MD3Log.h"
#include "platform/windows/Win32AppHost.h"
#include "platform/windows/Win32InputMapper.h"
#include "ui/panel/Md3Panel.h"

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

    // 将 stdout/stderr 重定向到 MD3::DebugLog（供共享 RenderMd3Panel 的 Log 区展示）
    // 说明：DebugStreamBuf 会同时把内容写回原始 streambuf，因此 DebugView/VS Output 里仍能看到。
    static MD3::DebugStreamBuf s_debugOut(std::cout.rdbuf(), MD3::LogLevel::Info);
    static MD3::DebugStreamBuf s_debugErr(std::cerr.rdbuf(), MD3::LogLevel::Error);
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
    // AppController 作为 AppState 单一真源；appState 作为其内部状态的引用别名，
    // 原有全部初始化/Init/WndProc/SaveSession 均共享这一份状态（D-015 Phase B）。
    ParticleSaturn::App::AppController controller{};
    AppState&                               appState = controller.MutableState();
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&backend));
    backend.SetController(&controller);

    // 标记是否支持透明效果（影响后续 SwapChain 选择和 UI 显示）
    appState.backdrop.transparentSupported = useDComp;

    // 初始化 AppState 基本值（对齐 OpenGL：默认粒子数为 MAX=1200000）
    appState.render.particleCount = 1200000;

    // 从注册表加载会话状态（覆盖默认值）
    Settings::LoadSession(appState);

    // 恢复窗口状态
    if (savedWindow.valid) {
        appState.window.windowedX    = savedWindow.x;
        appState.window.windowedY    = savedWindow.y;
        appState.window.windowedWidth    = savedWindow.w;
        appState.window.windowedHeight    = savedWindow.h;
        appState.window.fullscreen = savedWindow.fullscreen;
    }

    // 系统主题/窗口效果初始化
    {
        const bool dark        = ParticleSaturn::Win32WindowManager::IsSystemDarkMode();
        appState.ui.darkMode = dark;
        ParticleSaturn::Win32WindowManager::SetTitleBarDarkMode(hwnd, dark);
        std::cout << "[DWM] System theme: " << (dark ? "Dark" : "Light") << std::endl;

        ParticleSaturn::Win32WindowManager::DetectAvailableBackdrops(hwnd, appState);
        if (!appState.backdrop.availableBackdrops.empty()) {
            const int mode = appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex];
            ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd, mode, appState);
        }

        // 初始化 DPI 缩放
        appState.window.dpiScale = GetDpiScaleForWindow(hwnd);
        std::cout << "[Main] DPI scale: " << appState.window.dpiScale << std::endl;
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

    // D-015 Phase B（全对称上移）：手部追踪由外壳持有，与 macOS 外壳一致。后端不再
    // 引用 HandTracking::Controller，只经 FrameContext.handTracked 得知本帧有无手。
    // 模型随可执行文件分发（构建时把 HandTracker/models 复制到 exe 同目录）。
    ParticleSaturn::HandTracking::Controller handTracker;
    if (handTracker.Init(hwnd, &appState)) {
        // 非阻塞启动：后续每帧在 pollGesture() 里 Tick() 轮询就绪状态。
        handTracker.StartWithCameraSelector(false);
    }

    // D-002 帧高度接缝 / D-015 Phase B：Win32 外壳帧循环。
    // 外壳职责（接缝以上）：固定步长推进相机动画（FrameCoordinator，与 macOS 共享）、
    // FPS 度量（FpsMeter，D-001 单一实现）、每帧组装 FrameContext 交给后端渲染。
    // Diligent 后端保留其自有的 38/57 FPS 动态 LOD（densityCompensation + IndirectArgs），
    // 故此处关闭协调器 LOD，只借用其相机动画与平滑帧时度量。
    ParticleSaturn::App::FrameCoordinator coordinator;
    coordinator.SetLodEnabled(false);
    ParticleSaturn::App::FpsMeter fpsMeter;
    auto lastFrameTime = std::chrono::steady_clock::now();

    // 手部追踪 → 平台中立手势：轮询追踪器，就绪且有手时归一化原始 scale/rotX/rotY
    // （灵敏度/反转由 FrameCoordinator 统一施加，与 macOS 手势路径一致）。
    auto pollGesture = [&]() -> ParticleSaturn::App::GestureInput {
        ParticleSaturn::App::GestureInput gesture;
        handTracker.Tick();
        if (handTracker.GetStatus() == ParticleSaturn::HandTracking::Status::Ready) {
            const ParticleSaturn::HandTracking::Sample sample = handTracker.GetLatestSample();
            if (sample.hasHand) {
                gesture.tracked             = true;
                gesture.hasAbsolutePose     = true;
                gesture.scale               = sample.scale;
                gesture.rotationXNormalized = sample.rotX;
                gesture.rotationYNormalized = sample.rotY;
            }
        }
        return gesture;
    };

    // 面板由外壳绘制（接缝以上）：后端在其 ImGui 帧内回调 frame.drawPanel(BuildPanelHooks())，
    // 把每帧仅在渲染期有效的 acrylic 纹理经 hooks 交给外壳。外壳据此组装共享
    // RenderMd3Panel 的能力位/手部状态/回调——Windows 独有特性（后端切换、Mesh Shader、
    // 透明合成、SIMD、摄像头选择、追踪器调试/错误码/星数）全部由外壳按契约点亮。
    const std::function<void(const ParticleSaturn::App::BackendPanelHooks&)> drawPanel =
        [&](const ParticleSaturn::App::BackendPanelHooks& hooks) {
            using ParticleSaturn::Render::Backend;
            const Backend curBackend = backend.GetBackend();

            // 能力位 + Windows 扩展。
            ParticleSaturn::UI::Md3PanelBackendFeatures features;
            features.analyticParticles           = backend.Capabilities().analyticParticles;
            features.objectShaderParticles       = backend.Capabilities().objectShaderParticles;
            features.meshShaderSupported         = backend.MeshShaderSupported();
            features.meshShaderEnabled           = backend.MeshShaderEnabled();
            features.starCount                   = backend.StarCount();
            features.backendOptions              = {"D3D11", "D3D12", "Vulkan"};
            features.backendIndex                = static_cast<int>(curBackend);
            features.transparentBackdropSupported =
                appState.backdrop.transparentSupported &&
                (curBackend == Backend::D3D12 || curBackend == Backend::D3D11);
            features.transparentBackdropEnabled = appState.backdrop.useTransparent;

            // 手部追踪状态（由外壳持有的 handTracker 直接汇报）。
            using Tracker = ParticleSaturn::UI::Md3PanelHandTrackingStatus::Tracker;
            ParticleSaturn::UI::Md3PanelHandTrackingStatus handStatus;
            const ParticleSaturn::HandTracking::Status st = handTracker.GetStatus();
            switch (st) {
            case ParticleSaturn::HandTracking::Status::Ready:
                handStatus.tracker = Tracker::Ready;
                break;
            case ParticleSaturn::HandTracking::Status::Starting:
            case ParticleSaturn::HandTracking::Status::NotStarted:
                handStatus.tracker = Tracker::Initializing;
                break;
            case ParticleSaturn::HandTracking::Status::Failed:
                handStatus.tracker = Tracker::Failed;
                break;
            default:
                handStatus.tracker = Tracker::Unavailable;
                break;
            }
            handStatus.selectedCamera = handTracker.GetSelectedCamera();
            if (st == ParticleSaturn::HandTracking::Status::Failed) {
                handStatus.errorCode    = handTracker.GetLastErrorCode();
                handStatus.errorMessage = handTracker.GetLastErrorMessageUtf8();
            }
            if (st == ParticleSaturn::HandTracking::Status::Ready) {
                const ParticleSaturn::HandTracking::Sample sample = handTracker.GetLatestSample();
                handStatus.handDetected = sample.hasHand;
                handStatus.rawScale     = sample.scale;
                handStatus.rawRotX      = sample.rotX;
                handStatus.rawRotY      = sample.rotY;
            }
            int simdMode = 0;
            if (handTracker.GetSIMDMode(&simdMode)) {
                handStatus.simdSupported      = true;
                handStatus.simdMode           = simdMode;
                handStatus.simdImplementation = handTracker.GetSIMDImplementation();
            }
            bool debugEnabled = false;
            if (handTracker.GetDebugMode(&debugEnabled)) {
                handStatus.debugMode = debugEnabled;
            }

            // 面板回调（平台动作：外壳/后端/追踪器分发；未设置的控件由面板自动隐藏）。
            ParticleSaturn::UI::Md3PanelCallbacks callbacks;
            callbacks.save             = [&] { Settings::SaveSession(appState, backend.GetBackend()); };
            callbacks.toggleFullscreen = [&] {
                ParticleSaturn::Platform::Windows::ToggleFullscreen(hwnd, appState);
            };
            callbacks.showCameraSelector = [&] { handTracker.RestartWithCameraSelector(true); };
            callbacks.restartApplication = [&] {
                if (Settings::RestartWithBackend(backend.GetBackend(), appState)) {
                    PostQuitMessage(0);
                }
            };
            callbacks.setSimdMode         = [&](int mode) { handTracker.SetSIMDMode(mode); };
            callbacks.setHandDebugMode    = [&](bool enabled) { handTracker.SetDebugMode(enabled); };
            callbacks.setMeshShaderEnabled = [&](bool enabled) { backend.SetMeshShaderEnabled(enabled); };
            callbacks.switchBackend       = [&](int index) {
                const auto newBackend = static_cast<Backend>(index);
                if (newBackend != backend.GetBackend() && Settings::RestartWithBackend(newBackend, appState)) {
                    PostQuitMessage(0);
                }
            };
            callbacks.setTransparentBackdrop = [&](bool transparent) {
                const int newMode = transparent ? 3 : 0; // 透明=Mica，不透明=Solid
                if (backend.SetBackdropMode(newMode)) {
                    for (int i = 0; i < static_cast<int>(appState.backdrop.availableBackdrops.size()); ++i) {
                        if (appState.backdrop.availableBackdrops[i] == newMode) {
                            appState.backdrop.backdropIndex = i;
                            break;
                        }
                    }
                }
            };
            callbacks.drawAcrylicBackground = hooks.drawAcrylicBackground;
            callbacks.drawGraphAcrylic      = hooks.drawGraphAcrylic;

            const char* const backendName = (curBackend == Backend::D3D11)    ? "D3D11"
                                            : (curBackend == Backend::Vulkan) ? "Vulkan"
                                                                              : "D3D12";
            ParticleSaturn::UI::RenderMd3Panel(controller, backendName, fpsMeter.Value(), features, callbacks,
                                               handStatus);
        };

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // 退出前保存会话状态
                // 更新当前窗口位置（仅非全屏模式）
                if (!appState.window.fullscreen) {
                    RECT wr;
                    if (GetWindowRect(hwnd, &wr)) {
                        appState.window.windowedX = wr.left;
                        appState.window.windowedY = wr.top;
                        appState.window.windowedWidth = wr.right - wr.left;
                        appState.window.windowedHeight = wr.bottom - wr.top;
                    }
                }
                Settings::SaveSession(appState, requestedBackend);

                handTracker.Shutdown();
                backend.Shutdown();
                ErrorHandler::SetStage(ErrorHandler::AppStage::SHUTDOWN);
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // 1) 计算本帧 dt（限幅 [0,0.25]，与 macOS 外壳一致）。
        const auto now = std::chrono::steady_clock::now();
        const float deltaTime =
            std::clamp(std::chrono::duration<float>(now - lastFrameTime).count(), 0.0f, 0.25f);
        lastFrameTime = now;

        // 2) 手部追踪 → 归一化手势；固定步长推进相机动画（写入 state.scene.*）。
        const ParticleSaturn::App::GestureInput gesture = pollGesture();
        coordinator.Advance(controller, deltaTime, gesture);
        fpsMeter.AddSample(deltaTime);

        // 3) 组装本帧接缝上下文并交给后端渲染一帧。
        const auto client = GetClientSize(hwnd);
        appState.window.width  = client.Width;
        appState.window.height = client.Height;
        const ParticleSaturn::App::SurfaceSize drawable{client.Width, client.Height, appState.window.dpiScale};
        const ParticleSaturn::App::FrameContext context{appState,
                                                        deltaTime,
                                                        gesture.tracked,
                                                        gesture,
                                                        fpsMeter.Value(),
                                                        drawable,
                                                        appState.window.fullscreen,
                                                        drawPanel};
        backend.RenderFrame(context);
    }
}
