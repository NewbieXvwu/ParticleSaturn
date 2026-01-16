// Particle Saturn - 土星粒子系统
// GPU 粒子计算 + 动态 LOD + 手势追踪 + 实时渲染

#include "pch.h"

#ifdef EMBED_MODELS
#include "Resource.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <random>

#include "AppState.h"
#include "CameraSelector/CameraSelector.h"
#include "CrashAnalyzer.h"
#include "DebugLog.h"
#include "ErrorHandler.h"
#include "HandTracker.h"
#include "Localization.h"
#include "ParticleSystem.h"
#include "Renderer.h"
#include "Shaders.h"
#include "UIManager.h"
#include "Utils.h"
#include "WindowManager.h"
#include "generated/LogControlIcons.h"
#include "md3/MD3.h"

// 初始窗口尺寸常量
const unsigned int INIT_WIDTH  = 1920;
const unsigned int INIT_HEIGHT = 1080;

namespace {

static GLuint CreateTextureRGBA8(int w, int h, const unsigned char* rgba) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static GLuint CreateTextureFromAlphaMask(int px, const uint8_t* alpha, uint32_t rgb) {
    if (!alpha || px <= 0) {
        return 0;
    }
    unsigned char r = (unsigned char)((rgb >> 16) & 0xFF);
    unsigned char g = (unsigned char)((rgb >> 8) & 0xFF);
    unsigned char b = (unsigned char)(rgb & 0xFF);

    std::vector<unsigned char> rgba((size_t)px * (size_t)px * 4u, 0);
    for (int i = 0; i < px * px; i++) {
        rgba[(size_t)i * 4u + 0u] = r;
        rgba[(size_t)i * 4u + 1u] = g;
        rgba[(size_t)i * 4u + 2u] = b;
        rgba[(size_t)i * 4u + 3u] = alpha[i];
    }

    return CreateTextureRGBA8(px, px, rgba.data());
}

static GLuint GetOrCreateBakedLogIconTexture(bool pausedState /* true=resume icon, false=pause icon */) {
    static GLuint s_pauseTex  = 0;
    static GLuint s_resumeTex = 0;

    if (!pausedState) {
        if (s_pauseTex == 0) {
            s_pauseTex = CreateTextureFromAlphaMask(GeneratedIcons::kLogIconPx, GeneratedIcons::kLogPauseAlpha,
                                                    GeneratedIcons::kLogIconRgb);
        }
        return s_pauseTex;
    }

    if (s_resumeTex == 0) {
        s_resumeTex = CreateTextureFromAlphaMask(GeneratedIcons::kLogIconPx, GeneratedIcons::kLogResumeAlpha,
                                                 GeneratedIcons::kLogIconRgb);
    }
    return s_resumeTex;
}

} // namespace

// File drop callback for crash analyzer
void DropCallback(GLFWwindow* window, int count, const char** paths) {
    for (int i = 0; i < count; i++) {
        CrashAnalyzer::HandleFileDrop(paths[i]);
    }
}

int main() {
    // 创建应用程序状态
    AppState appState;
    appState.InitDefaults(MAX_PARTICLES);

    // Initialize error handler first
    ErrorHandler::Init();
    ErrorHandler::SetStage(ErrorHandler::AppStage::STARTUP);

    // 重定向 cout 到调试日志
    static DebugStreamBuf debugBuf(std::cout.rdbuf());
    std::cout.rdbuf(&debugBuf);

    std::cout << "[Main] Particle Saturn " << i18n::GetVersion() << " starting..." << std::endl;

    ErrorHandler::SetStage(ErrorHandler::AppStage::WINDOW_INIT);

    // 初始化 GLFW
    if (!glfwInit()) {
        std::cerr << "[Main] Fatal: glfwInit() failed" << std::endl;
        ErrorHandler::ShowEarlyFatalError(i18n::Get().glfwInitFailed, "glfwInit() returned false");
        return -1;
    }

    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    int initialClientW = (int)INIT_WIDTH;
    int initialClientH = (int)INIT_HEIGHT;

    int          workX   = 0;
    int          workY   = 0;
    int          workW   = 0;
    int          workH   = 0;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
        glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
#elif defined(_WIN32)
        RECT workArea{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
            workX = (int)workArea.left;
            workY = (int)workArea.top;
            workW = (int)(workArea.right - workArea.left);
            workH = (int)(workArea.bottom - workArea.top);
        }
#endif
        if (workW <= 0 || workH <= 0) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode) {
                workW = mode->width;
                workH = mode->height;
            }
        }

        if (workW > 0 && workH > 0) {
            const double scaleW = (double)workW / (double)INIT_WIDTH;
            const double scaleH = (double)workH / (double)INIT_HEIGHT;
            const double scale  = std::min(1.0, std::min(scaleW, scaleH));
            initialClientW      = (int)std::lround((double)INIT_WIDTH * scale);
            initialClientH      = (int)std::lround((double)INIT_HEIGHT * scale);
        }
    }

    // 创建窗口: 优先尝试 OpenGL 4.6 以获取持久映射支持, 失败则回退到 4.3
    // 某些驱动 (AMD/Intel) 只返回请求的版本而非设备支持的最高版本
    GLFWwindow* window               = nullptr;
    const int   glVersionsToTry[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}};
    for (const auto& ver : glVersionsToTry) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, ver[0]);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, ver[1]);
        window = glfwCreateWindow(initialClientW, initialClientH, "Particle Saturn", NULL, NULL);
        if (window) {
            std::cout << "[Main] OpenGL " << ver[0] << "." << ver[1] << " context created" << std::endl;
            break;
        }
    }
    if (!window) {
        std::cerr << "[Main] Fatal: glfwCreateWindow() failed" << std::endl;
        ErrorHandler::ShowEarlyFatalError(i18n::Get().windowCreateFailed, i18n::Get().detailWindowCreateFailed);
        glfwTerminate();
        return -1;
    }

    if (monitor && workW > 0 && workH > 0) {
        int frameL = 0, frameT = 0, frameR = 0, frameB = 0;
        glfwGetWindowFrameSize(window, &frameL, &frameT, &frameR, &frameB);

        const int maxClientW = workW - (frameL + frameR);
        const int maxClientH = workH - (frameT + frameB);
        if (maxClientW > 0 && maxClientH > 0) {
            const double scaleW = (double)maxClientW / (double)INIT_WIDTH;
            const double scaleH = (double)maxClientH / (double)INIT_HEIGHT;
            const double scale  = std::min(1.0, std::min(scaleW, scaleH));

            const int fittedClientW = (int)std::lround((double)INIT_WIDTH * scale);
            const int fittedClientH = (int)std::lround((double)INIT_HEIGHT * scale);
            if (fittedClientW != initialClientW || fittedClientH != initialClientH) {
                glfwSetWindowSize(window, fittedClientW, fittedClientH);
                initialClientW = fittedClientW;
                initialClientH = fittedClientH;
            }

            const int outerW = fittedClientW + frameL + frameR;
            const int outerH = fittedClientH + frameT + frameB;
            const int posX   = workX + std::max(0, (workW - outerW) / 2);
            const int posY   = workY + std::max(0, (workH - outerH) / 2);
            glfwSetWindowPos(window, posX, posY);
        }
    }

#ifdef _WIN32
    if (HWND hwnd = glfwGetWin32Window(window)) {
        RECT rect{};
        if (GetWindowRect(hwnd, &rect)) {
            HMONITOR    hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi)) {
                const int width  = (int)(rect.right - rect.left);
                const int height = (int)(rect.bottom - rect.top);

                int x = rect.left;
                int y = rect.top;

                if (y < mi.rcWork.top) {
                    y = mi.rcWork.top;
                }
                if (x < mi.rcWork.left) {
                    x = mi.rcWork.left;
                }
                if (x + width > mi.rcWork.right) {
                    x = mi.rcWork.right - width;
                }
                if (y + height > mi.rcWork.bottom) {
                    y = mi.rcWork.bottom - height;
                }
                if (x < mi.rcWork.left) {
                    x = mi.rcWork.left;
                }
                if (y < mi.rcWork.top) {
                    y = mi.rcWork.top;
                }

                if (x != rect.left || y != rect.top) {
                    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
        }
    }
#endif

    glfwMakeContextCurrent(window);

    // 加载 OpenGL 扩展
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "[Main] Fatal: gladLoadGLLoader() failed" << std::endl;
        ErrorHandler::ShowEarlyFatalError(i18n::Get().openglLoadFailed, i18n::Get().detailOpenGLLoadFailed);
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 验证 OpenGL 版本
    int glMajor, glMinor;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);
    if (glMajor < 4 || (glMajor == 4 && glMinor < 3)) {
        std::cerr << "[Main] Fatal: OpenGL " << glMajor << "." << glMinor << " < 4.3" << std::endl;
        std::ostringstream details;
        details << i18n::Get().detailOpenGLVersionLow << ": " << glMajor << "." << glMinor << "\n"
                << i18n::Get().detailOpenGLRequired << "\n\n"
                << "GPU: " << (const char*)glGetString(GL_RENDERER) << "\n"
                << "Driver: " << (const char*)glGetString(GL_VERSION);
        ErrorHandler::ShowEarlyFatalError(i18n::Get().openglVersionUnsupported, details.str().c_str());
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 初始化 VSync: 优先使用 Adaptive VSync，不支持时回退到传统 VSync
    appState.render.adaptiveVSyncSupported = glfwExtensionSupported("WGL_EXT_swap_control_tear");
    if (appState.render.adaptiveVSyncSupported) {
        appState.render.vsyncMode = -1; // Adaptive
        glfwSwapInterval(-1);
        std::cout << "[Main] VSync: Adaptive (WGL_EXT_swap_control_tear supported)" << std::endl;
    } else {
        appState.render.vsyncMode = 1; // On
        glfwSwapInterval(1);
        std::cout << "[Main] VSync: On (Adaptive not supported)" << std::endl;
    }

    // 设置 AppState 到窗口，供回调函数使用
    SetAppState(window, &appState);

    glfwSetFramebufferSizeCallback(window, WindowManager::FramebufferSizeCallback);
    glfwSetDropCallback(window, DropCallback);

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    WindowManager::FramebufferSizeCallback(window, fbW, fbH);
    glfwGetWindowSize(window, &appState.window.windowedW, &appState.window.windowedH);

    // Store OpenGL info for crash reports
    ErrorHandler::SetStage(ErrorHandler::AppStage::OPENGL_INIT);
    appState.gl.version  = (const char*)glGetString(GL_VERSION);
    appState.gl.renderer = (const char*)glGetString(GL_RENDERER);
    appState.gl.major    = glMajor;
    appState.gl.minor    = glMinor;
    // OpenGL 4.4+ 支持 Persistent Mapped Buffers
    appState.gl.persistentMapping = (glMajor > 4 || (glMajor == 4 && glMinor >= 4));
    ErrorHandler::SetGPUInfo(appState.gl.renderer, appState.gl.version);
    std::cout << "[Main] OpenGL: " << appState.gl.version
              << " (Persistent Mapping: " << (appState.gl.persistentMapping ? "Yes" : "No") << ")" << std::endl;

#ifdef _WIN32
    ImmAssociateContext(glfwGetWin32Window(window), NULL);

    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd) {
        WindowManager::SetTitleBarDarkMode(hwnd, true);
        appState.ui.isDarkMode = WindowManager::IsSystemDarkMode();
        std::cout << "[DWM] System theme: " << (appState.ui.isDarkMode ? "Dark" : "Light") << std::endl;
        WindowManager::InstallThemeChangeHook(hwnd);
        WindowManager::DetectAvailableBackdrops(hwnd, appState);
        appState.backdrop.backdropIndex = 0;
        WindowManager::SetBackdropMode(hwnd, appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex],
                                       appState);
    }
#endif

    // 初始化手部追踪（异步，不阻塞启动）
    ErrorHandler::SetStage(ErrorHandler::AppStage::HAND_TRACKER_INIT);
    bool handTrackerInitialized = false;
    bool handTrackerStarted     = false; // 追踪器线程是否已启动
    bool handTrackerCheckDone   = false; // 是否已完成初始化检查
#ifdef EMBED_MODELS
    std::cout << "[Main] Loading embedded models..." << std::endl;
    HRSRC hPalmRes = FindResource(NULL, MAKEINTRESOURCE(IDR_PALM_MODEL), RT_RCDATA);
    HRSRC hHandRes = FindResource(NULL, MAKEINTRESOURCE(IDR_HAND_MODEL), RT_RCDATA);
    if (!hPalmRes || !hHandRes) {
        std::cerr << "[Main] Warning: Failed to find embedded model resources" << std::endl;
        std::ostringstream details;
        details << "FindResource() failed:\n"
                << "  Palm model: " << (hPalmRes ? "Found" : "NOT FOUND") << "\n"
                << "  Hand model: " << (hHandRes ? "Found" : "NOT FOUND") << "\n\n"
                << "The executable may be corrupted or built incorrectly.";
        ErrorHandler::ShowWarning(i18n::Get().embeddedResourceFailed, details.str());
        handTrackerCheckDone = true; // 跳过后续检查
    } else {
        HGLOBAL hPalmData = LoadResource(NULL, hPalmRes);
        HGLOBAL hHandData = LoadResource(NULL, hHandRes);
        if (!hPalmData || !hHandData) {
            std::cerr << "[Main] Warning: Failed to load embedded model resources" << std::endl;
            ErrorHandler::ShowWarning(i18n::Get().embeddedResourceFailed, "LoadResource() failed");
            handTrackerCheckDone = true;
        } else {
            const void* palmData = LockResource(hPalmData);
            const void* handData = LockResource(hHandData);
            DWORD       palmSize = SizeofResource(NULL, hPalmRes);
            DWORD       handSize = SizeofResource(NULL, hHandRes);
            SetEmbeddedModels(palmData, palmSize, handData, handSize);
            std::cout << "[Main] Embedded models loaded (palm: " << palmSize << " bytes, hand: " << handSize
                      << " bytes)" << std::endl;
        }
    }
    if (!handTrackerCheckDone) {
        // 摄像头选择：如果有多个摄像头，弹出选择对话框
        HWND mainHwnd       = glfwGetWin32Window(window);
        int  selectedCamera = CameraSelector::ShowCameraSelectorDialog(mainHwnd);
        if (selectedCamera < 0) {
            // 用户取消，回退到默认摄像头 0
            std::cout << "[Main] Camera selection cancelled, falling back to camera 0" << std::endl;
            selectedCamera = 0;
        }
        {
            if (!InitTracker(selectedCamera, nullptr)) {
                std::cerr << "[Main] Warning: Failed to start HandTracker thread" << std::endl;
                ErrorHandler::ShowWarning(i18n::Get().cameraInitFailed,
                                          "InitTracker() returned false - thread creation failed");
                handTrackerCheckDone = true;
            } else {
                handTrackerStarted = true;
                std::cout << "[Main] HandTracker thread started with camera " << selectedCamera
                          << " (async initialization)" << std::endl;
            }
        }
    }
#else
    std::cout << "[Main] Initializing HandTracker (async)..." << std::endl;
    // 摄像头选择：如果有多个摄像头，弹出选择对话框
    HWND mainHwnd       = glfwGetWin32Window(window);
    int  selectedCamera = CameraSelector::ShowCameraSelectorDialog(mainHwnd);
    if (selectedCamera < 0) {
        // 用户取消，回退到默认摄像头 0
        std::cout << "[Main] Camera selection cancelled, falling back to camera 0" << std::endl;
        selectedCamera = 0;
    }
    {
        if (!InitTracker(selectedCamera, ".")) {
            std::cerr << "[Main] Warning: Failed to start HandTracker thread" << std::endl;
            ErrorHandler::ShowWarning(i18n::Get().cameraInitFailed,
                                      "InitTracker() returned false - thread creation failed");
            handTrackerCheckDone = true;
        } else {
            handTrackerStarted = true;
            std::cout << "[Main] HandTracker thread started with camera " << selectedCamera << " (async initialization)"
                      << std::endl;
        }
    }
#endif

    // UI 系统采用惰性加载策略：首次按 F3 打开调试窗口时才初始化
    // 这可以显著加快程序启动速度（节省字体加载和着色器编译时间）
    // appState.ui.imguiInitialized 标志用于跟踪初始化状态

    // 创建着色器程序
    ErrorHandler::SetStage(ErrorHandler::AppStage::SHADER_COMPILE);
    unsigned int pSaturn = Renderer::CreateProgram(Shaders::VertexSaturn, Shaders::FragmentSaturn);
    unsigned int pStar   = Renderer::CreateProgram(Shaders::VertexStar, Shaders::FragmentStar);
    unsigned int pUI     = Renderer::CreateProgram(Shaders::VertexUI, Shaders::FragmentUI);
    unsigned int pQuad   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentQuad);
    unsigned int pBlur   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentBlur);
    unsigned int pAcrylic = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentAcrylic);

    // 检查核心着色器是否编译成功
    if (!pSaturn || !pStar || !pUI || !pQuad || !pBlur || !pAcrylic) {
        std::cerr << "[Main] Fatal: Core shader compilation failed" << std::endl;
        std::ostringstream details;
        details << "Shader compilation status:\n"
                << "  pSaturn: " << (pSaturn ? "OK" : "FAILED") << "\n"
                << "  pStar:   " << (pStar ? "OK" : "FAILED") << "\n"
                << "  pUI:     " << (pUI ? "OK" : "FAILED") << "\n"
                << "  pQuad:   " << (pQuad ? "OK" : "FAILED") << "\n"
                << "  pBlur:   " << (pBlur ? "OK" : "FAILED") << "\n\n"
                << "  pAcrylic:" << (pAcrylic ? "OK" : "FAILED") << "\n\n"
                << "GPU: " << appState.gl.renderer << "\n"
                << "OpenGL: " << appState.gl.version;
        ErrorHandler::ShowError(i18n::Get().shaderCompileFailed, details.str());
        UIManager::Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 创建计算着色器
    unsigned int cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &Shaders::ComputeSaturn, 0);
    glCompileShader(cs);
    if (!Renderer::CheckShaderCompileStatus(cs, "Compute")) {
        std::cerr << "[Main] Fatal: Compute shader compilation failed" << std::endl;
        ErrorHandler::ShowError(i18n::Get().shaderCompileFailed, "Compute shader compilation failed");
        glDeleteShader(cs);
        UIManager::Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    unsigned int pComp = glCreateProgram();
    glAttachShader(pComp, cs);
    glLinkProgram(pComp);
    glDeleteShader(cs);

    if (!Renderer::CheckProgramLinkStatus(pComp)) {
        std::cerr << "[Main] Fatal: Compute program linking failed" << std::endl;
        ErrorHandler::ShowError(i18n::Get().shaderCompileFailed, "Compute shader program linking failed");
        glDeleteProgram(pComp);
        UIManager::Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 离屏渲染 FBO
    unsigned int fbo, fboTex, rbo;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &fboTex);
    glGenRenderbuffers(1, &rbo);

    // 优化: 使用 R11F_G11F_B10F 格式 (4字节/像素) 替代 RGBA16F (8字节/像素)
    // R11F_G11F_B10F 是紧凑的 HDR 格式，足够存储加法混合的高光值
    // 返回 true 表示成功，false 表示失败
    auto resizeFBO = [&](int width, int height) -> bool {
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[Main] FBO incomplete, status: 0x" << std::hex << status << std::dec << std::endl;
            return false;
        }
        return true;
    };

    if (!resizeFBO(appState.window.width, appState.window.height)) {
        std::cerr << "[Main] Fatal: Failed to create main framebuffer" << std::endl;
        std::ostringstream details;
        details << "glCheckFramebufferStatus() != GL_FRAMEBUFFER_COMPLETE\n\n"
                << "Resolution: " << appState.window.width << "x" << appState.window.height << "\n"
                << "GPU: " << appState.gl.renderer << "\n"
                << "OpenGL: " << appState.gl.version;
        ErrorHandler::ShowError(i18n::Get().fboCreateFailed, details.str());
        UIManager::Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 模糊效果 FBO
    // - 1/6：窗口背景强模糊
    // - 1/12：折叠区域弱模糊（对齐 Diligent 版的次级模糊链路）
    // - Acrylic：在模糊纹理上做饱和度/排除混合/着色合成，避免 ImGui 端叠加造成“二次着色”
    BlurFramebuffer fboBlur1, fboBlur2, fboBlur3, fboBlur4;
    BlurFramebuffer fboAcrylic1, fboAcrylic2;
    fboBlur1.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
    fboBlur2.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
    fboBlur3.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));
    fboBlur4.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));
    fboAcrylic1.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
    fboAcrylic2.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));

    // 噪点纹理（全分辨率；避免依赖 wrap sampler）
    GLuint noiseTex = 0;
    auto   rebuildNoiseTex = [&](int w, int h) {
        if (noiseTex != 0) {
            glDeleteTextures(1, &noiseTex);
            noiseTex = 0;
        }
        if (w <= 0 || h <= 0) {
            return;
        }

        std::vector<unsigned char> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
        std::mt19937                          gen{1337u};
        std::uniform_int_distribution<int>    rnd(0, 255);
        for (size_t i = 0; i < rgba.size(); i += 4u) {
            const unsigned char v = static_cast<unsigned char>(rnd(gen));
            rgba[i + 0u]          = v;
            rgba[i + 1u]          = v;
            rgba[i + 2u]          = v;
            rgba[i + 3u]          = 255u;
        }
        noiseTex = CreateTextureRGBA8(w, h, rgba.data());
    };
    rebuildNoiseTex(appState.window.width, appState.window.height);

    // 全屏四边形 VAO
    unsigned int vaoQuad, vboQuad;
    float        quadVerts[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vaoQuad);
    glGenBuffers(1, &vboQuad);
    glBindVertexArray(vaoQuad);
    glBindBuffer(GL_ARRAY_BUFFER, vboQuad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glBindVertexArray(0);

    // 初始化粒子系统 (双缓冲)
    ErrorHandler::SetStage(ErrorHandler::AppStage::PARTICLE_INIT);
    DoubleBufferSSBO particleBuffers;
    if (!ParticleSystem::InitParticlesGPU(particleBuffers)) {
        std::cerr << "[Main] Fatal: Failed to initialize particle system" << std::endl;
        // 检查是否是显存不足
        bool               isOutOfMemory = ParticleSystem::g_lastError.find("OUT_OF_MEMORY") != std::string::npos;
        const char*        message = isOutOfMemory ? i18n::Get().outOfVideoMemory : i18n::Get().shaderCompileFailed;
        std::ostringstream details;
        details << "ParticleSystem::InitParticlesGPU() failed\n\n"
                << ParticleSystem::g_lastError << "\n\n"
                << "GPU: " << appState.gl.renderer << "\n"
                << "OpenGL: " << appState.gl.version;
        ErrorHandler::ShowError(message, details.str());
        UIManager::Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 创建星空背景
    unsigned int vaoStars, vboStars;
    ParticleSystem::CreateStars(vaoStars, vboStars);

    // 预生成数字几何 (FPS 显示优化)
    Renderer::PrebuiltDigits prebuiltDigits;
    prebuiltDigits.Init();

    glEnable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glDepthMask(GL_FALSE);

    // 初始化 Uniform 缓存
    UniformCache uc;
    Renderer::InitUniformCache(uc, pComp, pSaturn, pStar, pUI, pBlur, pAcrylic, pQuad);

    // 投影和视图矩阵
    glm::mat4 proj   = glm::perspective(1.047f, (float)appState.window.width / appState.window.height, 1.f, 10000.f);
    glm::mat4 view   = glm::lookAt(glm::vec3(0, 0, 100), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 projUI = glm::ortho(0.0f, (float)appState.window.width, 0.0f, (float)appState.window.height);

    // 动画状态
    SmoothState currentAnim;
    float       autoTime = 0;

    // 异步手部追踪器 (优化: 消除主线程阻塞)
    // 注意: asyncTracker.Start() 会在主循环中检测到 HandTracker 初始化成功后调用
    AsyncHandTracker asyncTracker;

    // 主循环变量
    float             lastFrame  = 0;
    float             currentFps = 60.0f;
    RingBufferFPS<60> fpsCalculator;         // 优化: 使用环形缓冲区计算平滑 FPS
    float             lodUpdateTimer = 0.0f; // LOD 更新计时器

    // 主渲染循环
    ErrorHandler::SetStage(ErrorHandler::AppStage::RENDER_LOOP);
    int totalFrameCount = 0;
    while (!glfwWindowShouldClose(window)) {
        float t   = (float)glfwGetTime();
        float dt  = t - lastFrame;
        lastFrame = t;

        // 异步检查 HandTracker 初始化状态（非阻塞）
        if (handTrackerStarted && !handTrackerCheckDone) {
            int readyStatus = IsTrackerReady();
            if (readyStatus != 0) {
                // 初始化完成（成功或失败）
                handTrackerCheckDone = true;
                if (readyStatus == 1) {
                    std::cout << "[Main] HandTracker initialized successfully." << std::endl;
                    handTrackerInitialized = true;
                    asyncTracker.Start();
                    ErrorHandler::SetCameraInfo(0, 640, 480, true);
                } else {
                    std::cerr << "[Main] Warning: HandTracker initialization failed" << std::endl;
                    int         errCode = GetTrackerLastError();
                    const char* errMsg  = GetTrackerLastErrorMessage();
                    const char* localizedMsg;
                    switch (errCode) {
                    case HANDTRACKER_ERROR_PALM_MODEL:
                        localizedMsg = i18n::Get().palmModelLoadFailed;
                        break;
                    case HANDTRACKER_ERROR_HAND_MODEL:
                        localizedMsg = i18n::Get().handModelLoadFailed;
                        break;
                    case HANDTRACKER_ERROR_NO_CAMERA:
                        localizedMsg = i18n::Get().cameraNotFound;
                        break;
                    case HANDTRACKER_ERROR_CAMERA_IN_USE:
                        localizedMsg = i18n::Get().cameraInUse;
                        break;
                    default:
                        localizedMsg = i18n::Get().cameraInitFailed;
                        break;
                    }
                    ErrorHandler::ShowWarning(localizedMsg, errMsg ? errMsg : "Initialization failed");
                }
            }
        }

        // MD3 帧开始（仅在 UI 已初始化时）
        if (appState.ui.imguiInitialized) {
            MD3::BeginFrame(dt);
        }

        // 处理窗口大小变化
        if (appState.window.resized) {
            appState.window.resized = false;
            proj   = glm::perspective(1.047f, (float)appState.window.width / appState.window.height, 1.f, 10000.f);
            projUI = glm::ortho(0.0f, (float)appState.window.width, 0.0f, (float)appState.window.height);
            resizeFBO(appState.window.width, appState.window.height);
            fboBlur1.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
            fboBlur2.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
            fboBlur3.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));
            fboBlur4.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));
            fboAcrylic1.Init(std::max(1, appState.window.width / 6), std::max(1, appState.window.height / 6));
            fboAcrylic2.Init(std::max(1, appState.window.width / 12), std::max(1, appState.window.height / 12));
            rebuildNoiseTex(appState.window.width, appState.window.height);
            if (appState.ui.imguiInitialized) {
                MD3::SetScreenSize((float)appState.window.width, (float)appState.window.height);
            }
        }

        // 获取手部追踪数据 (异步: 非阻塞读取最新状态)
        HandState handState = asyncTracker.GetLatestState();

        // 优化: 使用环形缓冲区计算平滑 FPS
        fpsCalculator.AddFrameTime(dt);
        currentFps = fpsCalculator.GetAverageFPS();

        // 动态 LOD 调整 (每 0.5 秒检查一次)
        lodUpdateTimer += dt;
        if (lodUpdateTimer >= 0.5f && !appState.lod.locked) {
            lodUpdateTimer = 0.0f;

            float smoothedFps          = currentFps; // 环形缓冲区已经提供平滑值
            bool  particleCountChanged = false;
            bool  pixelRatioChanged    = false;

            // 扩展滞后区间: 38-57 FPS 进一步减少边界震荡
            if (smoothedFps < 38.0f) {
                // 更保守的降质策略: 0.95 替代 0.9
                if (appState.render.activeParticleCount > MIN_PARTICLES) {
                    appState.render.activeParticleCount = (unsigned int)(appState.render.activeParticleCount * 0.95f);
                    appState.render.activeParticleCount = std::max(appState.render.activeParticleCount, MIN_PARTICLES);
                    particleCountChanged                = true;
                    appState.lod.lastDecision           = 1; // 降低粒子数
                } else if (appState.render.pixelRatio > 0.7f) {
                    appState.render.pixelRatio -= 0.03f;
                    appState.render.pixelRatio = std::max(appState.render.pixelRatio, 0.7f);
                    pixelRatioChanged          = true;
                    appState.lod.lastDecision  = 2; // 降低像素比例
                }
            } else if (smoothedFps > 57.0f) {
                // 更保守的提质策略: 1.05 替代 1.1
                if (appState.render.pixelRatio < 1.0f) {
                    appState.render.pixelRatio += 0.03f;
                    appState.render.pixelRatio = std::min(appState.render.pixelRatio, 1.0f);
                    pixelRatioChanged          = true;
                    appState.lod.lastDecision  = 3; // 提高像素比例
                } else if (appState.render.activeParticleCount < MAX_PARTICLES) {
                    appState.render.activeParticleCount = (unsigned int)(appState.render.activeParticleCount * 1.05f);
                    appState.render.activeParticleCount = std::min(appState.render.activeParticleCount, MAX_PARTICLES);
                    particleCountChanged                = true;
                    appState.lod.lastDecision           = 4; // 提高粒子数
                }
            } else {
                appState.lod.lastDecision = 0; // 稳定
            }

            // 更新 Indirect Draw Buffer 中的粒子数量
            if (particleCountChanged) {
                glBindBuffer(GL_DRAW_INDIRECT_BUFFER, particleBuffers.GetIndirectBuffer());
                glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(unsigned int), &appState.render.activeParticleCount);
            }

            // 优化: 只在粒子数或像素比例变化时重新计算密度补偿
            if (particleCountChanged || pixelRatioChanged) {
                float ratio                 = (float)appState.render.activeParticleCount / MAX_PARTICLES;
                appState.render.densityComp = 0.6f / pow(ratio, 0.7f) / pow(appState.render.pixelRatio, 0.5f);
            }
        } else if (lodUpdateTimer >= 0.5f) {
            lodUpdateTimer = 0.0f; // 即使锁定也要重置计时器
        }

        // 动画逻辑
        float targetScale, targetRotX, targetRotY;
        if (!handState.hasHand) {
            // 改为与帧率无关：保持“当前 180fps 下”的播放速度。
            // 旧版：autoTime 每帧 +0.005 -> 180fps 等价每秒 +0.9
            const float fixedRate = 0.005f * 180.0f;
            autoTime += dt * fixedRate;
            targetScale       = 1.0f + sin(autoTime) * 0.2f;
            targetRotX        = 0.4f + sin(autoTime * 0.3f) * 0.15f;
            targetRotY        = 0.0f;

            // 旧版：每帧 lerpFactor=0.08；转换成 dt 下的等效 alpha，参考帧率取 180fps。
            const float perFrameAlpha = 0.08f;
            const float alpha         = 1.0f - powf(1.0f - perFrameAlpha, dt * 180.0f);

            currentAnim.scale = Lerp(currentAnim.scale, targetScale, alpha);
            currentAnim.rotX  = Lerp(currentAnim.rotX, targetRotX, alpha);
            currentAnim.rotY  = Lerp(currentAnim.rotY, targetRotY, alpha);
        } else {
            targetScale = handState.scale;
            targetRotX  = -0.6f + handState.rotY * 1.6f;
            targetRotY  = (handState.rotX - 0.5f) * 2.0f;
            // 使用插值平滑过渡，避免 30fps 摄像头数据在 90fps 渲染时的跳变
            const float perFrameAlpha = 0.25f;
            const float alpha         = 1.0f - powf(1.0f - perFrameAlpha, dt * 180.0f);
            currentAnim.scale         = Lerp(currentAnim.scale, targetScale, alpha);
            currentAnim.rotX          = Lerp(currentAnim.rotX, targetRotX, alpha);
            currentAnim.rotY          = Lerp(currentAnim.rotY, targetRotY, alpha);
        }

        // 计算粒子物理 (双缓冲: 从当前缓冲读取，写入另一个缓冲)
        glUseProgram(pComp);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particleBuffers.GetReadSSBO());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, particleBuffers.GetWriteSSBO());
        glUniform1f(uc.comp_uDt, dt);
        glUniform1f(uc.comp_uHandScale, currentAnim.scale);
        glUniform1f(uc.comp_uHandHas, handState.hasHand ? 1.0f : 0.0f);
        glUniform1ui(uc.comp_uParticleCount, appState.render.activeParticleCount);
        glDispatchCompute((appState.render.activeParticleCount + 255) / 256, 1, 1);
        // 交换缓冲，下一帧渲染刚写入的数据
        particleBuffers.Swap();
        // 优化: 使用更精确的内存屏障组合
        // GL_SHADER_STORAGE_BARRIER_BIT: 确保 SSBO 写入完成
        // GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT: 确保顶点属性读取可见
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        // 渲染到 FBO
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        glm::mat4 mSat = glm::mat4(1.f);
        mSat           = glm::rotate(mSat, currentAnim.rotX, glm::vec3(1, 0, 0));
        mSat           = glm::rotate(mSat, currentAnim.rotY, glm::vec3(0, 1, 0));
        mSat           = glm::rotate(mSat, 0.466f, glm::vec3(0, 0, 1));

        // 渲染星空 (优化: 根据像素比例动态调整星星数量)
        glUseProgram(pStar);
        glUniformMatrix4fv(uc.star_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.star_view, 1, 0, &view[0][0]);
        glm::mat4 mStar = glm::rotate(glm::mat4(1.f), t * 0.005f, glm::vec3(0, 1, 0));
        glUniformMatrix4fv(uc.star_model, 1, 0, &mStar[0][0]);
        glUniform1f(uc.star_uTime, t);
        glBindVertexArray(vaoStars);
        // 星空 LOD: 低分辨率时减少星星数量 (对视觉影响极小)
        unsigned int starLODCount = (appState.render.pixelRatio < 0.85f)
                                      ? (unsigned int)(STAR_COUNT * 0.6f) // 60% 星星在低分辨率模式
                                      : STAR_COUNT;
        glDrawArrays(GL_POINTS, 0, starLODCount);

        // 渲染土星粒子 (使用 Indirect Drawing 消除 CPU 开销)
        glUseProgram(pSaturn);
        glUniformMatrix4fv(uc.sat_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.sat_view, 1, 0, &view[0][0]);
        glUniformMatrix4fv(uc.sat_model, 1, 0, &mSat[0][0]);
        glUniform1f(uc.sat_uTime, t);
        glUniform1f(uc.sat_uScale, currentAnim.scale);
        glUniform1f(uc.sat_uPixelRatio, appState.render.pixelRatio);
        glUniform1f(uc.sat_uDensityComp, appState.render.densityComp); // 使用缓存值，避免每帧计算
        glUniform1f(uc.sat_uScreenHeight, (float)appState.window.height);
        glBindVertexArray(particleBuffers.GetRenderVAO());
        // 使用 Indirect Drawing: GPU 直接读取绘制参数，减少 CPU-GPU 同步
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, particleBuffers.GetIndirectBuffer());
        glDrawArraysIndirect(GL_POINTS, nullptr);

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        // 渲染 FPS 显示 (使用预生成数字几何，无需每帧重建)
        glUseProgram(pUI);
        glUniformMatrix4fv(uc.ui_proj, 1, 0, &projUI[0][0]);
        glm::vec3 fpsCol = (currentFps > 50)
                             ? glm::vec3(0.3, 1.0, 0.3)
                             : ((currentFps > 30) ? glm::vec3(1.0, 0.6, 0.0) : glm::vec3(1.0, 0.2, 0.2));
        glUniform3fv(uc.ui_uColor, 1, &fpsCol[0]);
        glLineWidth(2.0f);

        // 使用预生成数字渲染 FPS
        // 优化: 使用栈上 char 数组避免每帧 std::string 堆分配
        int   displayFps = (int)currentFps;
        char  fpsBuffer[8];
        int   fpsLen  = snprintf(fpsBuffer, sizeof(fpsBuffer), "%d", displayFps);
        float xCursor = (float)appState.window.width - 60.0f;
        float numSize = 20.0f;
        for (int i = fpsLen - 1; i >= 0; i--) {
            prebuiltDigits.DrawDigit(fpsBuffer[i] - '0', xCursor, (float)appState.window.height - 40, numSize,
                                     uc.ui_uTransform);
            xCursor -= (numSize + 10.0f);
        }

        // 模糊处理 (Kawase Blur - 更高效的模糊算法)
        // 优化: 预先计算迭代次数，确保最终结果在 fboBlur2 中，避免额外的复制 pass
        GLuint finalBlurTex  = fboBlur2.tex; // 1/6 强模糊结果
        GLuint finalBlurTex2 = fboBlur3.tex; // 1/12 弱模糊结果（用于折叠区域）
        if (appState.ui.enableBlur) {
            glBlendFunc(GL_ONE, GL_ZERO);
            glViewport(0, 0, fboBlur1.w, fboBlur1.h);
            glUseProgram(pBlur);
            glUniform1i(uc.blur_uTexture, 0);
            glUniform2f(uc.blur_uTexelSize, 1.0f / fboBlur1.w, 1.0f / fboBlur1.h);
            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(vaoQuad);

            // Kawase Blur: 每次迭代增加采样偏移
            int   iterations    = 3 + (int)appState.ui.blurStrength;
            float offsets[]     = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
            int   maxIterations = sizeof(offsets) / sizeof(offsets[0]);
            iterations          = (iterations > maxIterations) ? maxIterations : iterations;

            // 优化: 调整迭代次数为偶数，确保最终结果自然落在 fboBlur2 中
            // 这样避免了原来的额外复制 pass
            if (iterations % 2 == 1) {
                iterations++; // 增加一次迭代比复制更有意义（额外模糊效果）
                if (iterations > maxIterations) {
                    iterations = maxIterations;
                }
            }

            // 第一次: fboTex -> fboBlur1
            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
            glBindTexture(GL_TEXTURE_2D, fboTex);
            glUniform1f(uc.blur_uOffset, offsets[0]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // 后续迭代: ping-pong between fboBlur1 and fboBlur2
            // 奇数次迭代写入 fboBlur2，偶数次迭代写入 fboBlur1
            // 由于 iterations 是偶数，最后一次 (iterations-1) 是奇数，写入 fboBlur2
            for (int i = 1; i < iterations; i++) {
                if (i % 2 == 1) {
                    glBindFramebuffer(GL_FRAMEBUFFER, fboBlur2.fbo);
                    glBindTexture(GL_TEXTURE_2D, fboBlur1.tex);
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
                    glBindTexture(GL_TEXTURE_2D, fboBlur2.tex);
                }
                glUniform1f(uc.blur_uOffset, offsets[std::min(i, maxIterations - 1)]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            // 最终结果现在保证在 fboBlur2 中
            finalBlurTex = fboBlur2.tex;

            // ========== 次级模糊（1/12 分辨率，用于折叠区域）==========
            // 第一步：从 1/6 降采样到 1/12（复用 blur shader，offset=0 作为 4-tap box downsample）
            glViewport(0, 0, fboBlur3.w, fboBlur3.h);
            glUniform2f(uc.blur_uTexelSize, 1.0f / fboBlur2.w, 1.0f / fboBlur2.h); // 源纹理 texel size

            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur3.fbo);
            glBindTexture(GL_TEXTURE_2D, fboBlur2.tex);
            glUniform1f(uc.blur_uOffset, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // 第二步：2 次小 offset 模糊（ping-pong，最终落回 fboBlur3）
            glUniform2f(uc.blur_uTexelSize, 1.0f / fboBlur3.w, 1.0f / fboBlur3.h);
            float secondaryOffsets[] = {0.5f, 1.0f};
            for (int i = 0; i < 2; ++i) {
                if (i % 2 == 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, fboBlur4.fbo);
                    glBindTexture(GL_TEXTURE_2D, fboBlur3.tex);
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, fboBlur3.fbo);
                    glBindTexture(GL_TEXTURE_2D, fboBlur4.tex);
                }
                glUniform1f(uc.blur_uOffset, secondaryOffsets[i]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            finalBlurTex2 = fboBlur3.tex;

            // ========== Acrylic 合成（饱和度增强 + 近似 exclusion + tint）==========
            // 在低分辨率模糊纹理上合成，避免 ImGui 端二次叠加造成偏色/灰蒙。
            glUseProgram(pAcrylic);
            glUniform1i(uc.acrylic_uTexture, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(vaoQuad);

            const bool isDark = appState.ui.isDarkMode;

            // Strong (1/6)：用于窗口背景
            glViewport(0, 0, fboAcrylic1.w, fboAcrylic1.h);
            glBindFramebuffer(GL_FRAMEBUFFER, fboAcrylic1.fbo);
            glBindTexture(GL_TEXTURE_2D, finalBlurTex);
            if (isDark) {
                glUniform4f(uc.acrylic_uTint, 20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 180.0f / 255.0f);
            } else {
                glUniform4f(uc.acrylic_uTint, 245.0f / 255.0f, 245.0f / 255.0f, 255.0f / 255.0f, 150.0f / 255.0f);
            }
            glUniform4f(uc.acrylic_uParams, 1.35f, 0.35f, isDark ? 1.0f : 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // Weak (1/12)：用于折叠区域
            glViewport(0, 0, fboAcrylic2.w, fboAcrylic2.h);
            glBindFramebuffer(GL_FRAMEBUFFER, fboAcrylic2.fbo);
            glBindTexture(GL_TEXTURE_2D, finalBlurTex2);
            if (isDark) {
                glUniform4f(uc.acrylic_uTint, 35.0f / 255.0f, 35.0f / 255.0f, 40.0f / 255.0f, 160.0f / 255.0f);
            } else {
                glUniform4f(uc.acrylic_uTint, 250.0f / 255.0f, 250.0f / 255.0f, 255.0f / 255.0f, 140.0f / 255.0f);
            }
            glUniform4f(uc.acrylic_uParams, 1.30f, 0.30f, isDark ? 1.0f : 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            finalBlurTex  = fboAcrylic1.tex;
            finalBlurTex2 = fboAcrylic2.tex;

            glViewport(0, 0, appState.window.width, appState.window.height);
        }

        // 合成到屏幕
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (appState.backdrop.useTransparent) {
            glClearColor(0, 0, 0, 0);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glClearColor(0, 0, 0, 1);
            glBlendFunc(GL_ONE, GL_ZERO);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glUseProgram(pQuad);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glUniform1i(uc.quad_uTexture, 0);
        glUniform1f(uc.quad_uTransparent, appState.backdrop.useTransparent ? 1.0f : 0.0f);
        glBindVertexArray(vaoQuad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Update error handler state
        totalFrameCount++;
        ErrorHandler::UpdateState(totalFrameCount, appState.render.activeParticleCount, appState.render.pixelRatio,
                                  handState.hasHand);

        // 渲染 ImGui（仅在已初始化时）
        if (appState.ui.imguiInitialized) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // 传递 Acrylic 合成纹理给 MD3（已包含：饱和度增强 + 近似 exclusion + tint 调制）
            MD3::SetBlurTexture(appState.ui.enableBlur ? fboAcrylic1.tex : 0, appState.ui.enableBlur);
            MD3::SetBlurTexture2(appState.ui.enableBlur ? fboAcrylic2.tex : 0);
            MD3::SetNoiseTexture(appState.ui.enableBlur ? noiseTex : 0);

            // 平滑滚动：必须在提交任何窗口内容之前跑一次（否则这一帧改 ScrollY 没意义）
            MD3::HandleSmoothScroll(60.0f);

            // Render error dialogs
            ErrorHandler::RenderErrorDialog(dt);

            // Render crash analyzer window
            CrashAnalyzer::Render(appState.ui.enableBlur, fboAcrylic1.tex, appState.window.width, appState.window.height,
                                  appState.ui.isDarkMode);

            if (appState.ui.showDebugWindow) {
                const auto& str = i18n::Get();
                ImGui::SetNextWindowPos(ImVec2(20 * appState.ui.dpiScale, 10 * appState.ui.dpiScale),
                                        ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(450 * appState.ui.dpiScale, 600 * appState.ui.dpiScale),
                                         ImGuiCond_FirstUseEver);
                ImGuiStyle& style            = ImGui::GetStyle();
                ImVec4      originalWindowBg = style.Colors[ImGuiCol_WindowBg];

                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
                ImGui::Begin(str.debugPanelTitle, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollWithMouse);

                ImVec2      pos  = ImGui::GetWindowPos();
                ImVec2      size = ImGui::GetWindowSize();
                ImDrawList* dl   = ImGui::GetWindowDrawList();

                if (appState.ui.enableBlur) {
                    ImVec2 uv0 = ImVec2(pos.x / appState.window.width, 1.0f - pos.y / appState.window.height);
                    ImVec2 uv1 = ImVec2((pos.x + size.x) / appState.window.width,
                                        1.0f - (pos.y + size.y) / appState.window.height);
                    // 使用带圆角的图片绘制，避免黑边
                    MD3::AddImageRounded(dl, fboAcrylic1.tex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1,
                                         IM_COL32(255, 255, 255, 255), style.WindowRounding);
                    if (noiseTex != 0) {
                        const ImU32 noiseCol =
                            appState.ui.isDarkMode ? IM_COL32(255, 255, 255, 10) : IM_COL32(255, 255, 255, 8);
                        MD3::AddImageRounded(dl, noiseTex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1,
                                             noiseCol, style.WindowRounding);
                    }
                    ImU32 highlight =
                        appState.ui.isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
                } else {
                    // Use the saved original background color instead of the overridden transparent one
                    ImVec4 bgCol = originalWindowBg;
                    bgCol.w      = 0.95f;
                    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol),
                                      style.WindowRounding);
                }
                ImGui::PopStyleColor(4);

                // 为标题栏预留空间
                MD3::WindowTitleBarSpace();

                if (MD3::BeginCollapsingHeader(str.sectionPerformance, true)) {
                    // 两列布局的辅助 lambda
                    auto TwoColumnText = [](const char* label, const char* fmt, ...) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", label);
                        ImGui::TableNextColumn();
                        va_list args;
                        va_start(args, fmt);
                        char buf[128];
                        vsnprintf(buf, sizeof(buf), fmt, args);
                        va_end(args);
                        ImGui::Text("%s", buf);
                    };

                    if (ImGui::BeginTable("PerfTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100 * appState.ui.dpiScale);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                        // FPS（带颜色）
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.fps);
                        ImGui::TableNextColumn();
                        ImVec4 fpsColor = (currentFps >= 50.0f) ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                        : (currentFps >= 30.0f) ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                                                : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        ImGui::TextColored(fpsColor, "%.1f", currentFps);

                        TwoColumnText(str.particles, "%u / %u", appState.render.activeParticleCount, MAX_PARTICLES);
                        TwoColumnText(str.pixelRatio, "%.2f", appState.render.pixelRatio);
                        TwoColumnText(str.resolution, "%u x %u", appState.window.width, appState.window.height);
                        TwoColumnText(str.openglVersion, "%d.%d%s", appState.gl.major, appState.gl.minor,
                                      appState.gl.persistentMapping ? "" : " (compat)");

                        ImGui::EndTable();
                    }

                    // FPS 历史曲线（低频采样，避免高刷屏幕上更新太快）
                    // 使用自定义绘制实现 Catmull-Rom 平滑曲线 + 滚动动画
                    ImGui::Dummy(ImVec2(0, 5));
                    const float* displayHistory = fpsCalculator.GetDisplayHistory();
                    int          historySize    = fpsCalculator.GetDisplayHistorySize();
                    int          currentIdx     = fpsCalculator.GetDisplayHistoryIndex();

                    // 获取滚动动画进度 (0~1)
                    float scrollProgress = fpsCalculator.GetScrollProgress();

                    // 辅助函数：获取环形缓冲区中的值（带边界处理）
                    auto getValue = [&](int logicalIdx) -> float {
                        // logicalIdx: 0 = 最旧, historySize-1 = 最新
                        int actualIdx = (currentIdx + logicalIdx) % historySize;
                        return displayHistory[actualIdx];
                    };

                    // 计算目标 Y 轴范围（根据当前数据自适应）
                    float dataMin = getValue(0);
                    float dataMax = getValue(0);
                    for (int i = 1; i < historySize; i++) {
                        float v = getValue(i);
                        if (v < dataMin) {
                            dataMin = v;
                        }
                        if (v > dataMax) {
                            dataMax = v;
                        }
                    }

                    // 设置最小显示范围（放大上限），防止稳定时过度放大
                    const float MIN_DISPLAY_RANGE = 30.0f;
                    float       dataRange         = dataMax - dataMin;
                    if (dataRange < MIN_DISPLAY_RANGE) {
                        float center = (dataMax + dataMin) * 0.5f;
                        dataMin      = center - MIN_DISPLAY_RANGE * 0.5f;
                        dataMax      = center + MIN_DISPLAY_RANGE * 0.5f;
                    }

                    // 添加 10% 的边距
                    float margin       = (dataMax - dataMin) * 0.1f;
                    float targetMinVal = dataMin - margin;
                    float targetMaxVal = dataMax + margin;
                    if (targetMinVal < 0.0f) {
                        targetMinVal = 0.0f;
                    }

                    // Y 轴范围平滑动画（使用静态变量保持状态）
                    static float animMinVal = 60.0f;
                    static float animMaxVal = 120.0f;
                    const float  animSpeed  = 8.0f; // 动画速度
                    float        dt         = ImGui::GetIO().DeltaTime;
                    animMinVal += (targetMinVal - animMinVal) * (1.0f - std::exp(-animSpeed * dt));
                    animMaxVal += (targetMaxVal - animMaxVal) * (1.0f - std::exp(-animSpeed * dt));
                    // 接近目标时直接到达，避免无限逼近
                    if (std::abs(targetMinVal - animMinVal) < 0.1f) {
                        animMinVal = targetMinVal;
                    }
                    if (std::abs(targetMaxVal - animMaxVal) < 0.1f) {
                        animMaxVal = targetMaxVal;
                    }

                    float minVal   = animMinVal;
                    float maxVal   = animMaxVal;
                    float valRange = maxVal - minVal;
                    if (valRange < 1.0f) {
                        valRange = 1.0f;
                    }

                    // 绘图区域设置（全宽，Y轴刻度作为overlay）
                    ImVec2      plotSize(ImGui::GetContentRegionAvail().x, 50);
                    ImVec2      plotPos  = ImGui::GetCursorScreenPos();
                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                    // 背景
                    ImU32 bgColor   = ImGui::GetColorU32(ImGuiCol_FrameBg);
                    ImU32 lineColor = ImGui::GetColorU32(ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
                    ImU32 axisColor = ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.6f, 0.9f));
                    drawList->AddRectFilled(plotPos, ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y), bgColor);

                    // 添加裁剪区域，防止曲线超出边界
                    drawList->PushClipRect(plotPos, ImVec2(plotPos.x + plotSize.x, plotPos.y + plotSize.y), true);

                    // 辅助函数：将逻辑索引和 FPS 值转换为屏幕坐标
                    auto toScreen = [&](float logicalX, float val) -> ImVec2 {
                        float adjustedX  = logicalX - scrollProgress;
                        float x          = plotPos.x + (adjustedX / (float)(historySize - 1)) * plotSize.x;
                        float clampedVal = val < minVal ? minVal : (val > maxVal ? maxVal : val);
                        float y          = plotPos.y + plotSize.y - ((clampedVal - minVal) / valRange) * plotSize.y;
                        return ImVec2(x, y);
                    };

                    // 使用 Catmull-Rom 插值绘制平滑曲线
                    const int        subdivisions = 4;
                    ImVector<ImVec2> points;
                    points.reserve((historySize - 1) * subdivisions + 1);

                    for (int i = 0; i < historySize - 1; i++) {
                        float p0 = getValue(i > 0 ? i - 1 : 0);
                        float p1 = getValue(i);
                        float p2 = getValue(i + 1);
                        float p3 = getValue(i + 2 < historySize ? i + 2 : historySize - 1);

                        for (int j = 0; j < subdivisions; j++) {
                            float  t        = (float)j / subdivisions;
                            float  val      = CatmullRom(p0, p1, p2, p3, t);
                            float  logicalX = (float)i + t;
                            ImVec2 pt       = toScreen(logicalX, val);

                            if (pt.x >= plotPos.x - 5 && pt.x <= plotPos.x + plotSize.x + 5) {
                                points.push_back(pt);
                            }
                        }
                    }
                    // 添加最后一个点
                    {
                        float  lastVal = getValue(historySize - 1);
                        ImVec2 lastPt  = toScreen((float)(historySize - 1), lastVal);
                        if (lastPt.x >= plotPos.x - 5 && lastPt.x <= plotPos.x + plotSize.x + 5) {
                            points.push_back(lastPt);
                        }
                    }

                    // 绘制曲线
                    if (points.Size >= 2) {
                        drawList->AddPolyline(points.Data, points.Size, lineColor, ImDrawFlags_None, 1.5f);
                    }

                    // 恢复裁剪区域
                    drawList->PopClipRect();

                    // Y 轴刻度作为 overlay（绘制在曲线之上）
                    char maxLabel[16], minLabel[16];
                    snprintf(maxLabel, sizeof(maxLabel), "%.0f", maxVal);
                    snprintf(minLabel, sizeof(minLabel), "%.0f", minVal);
                    // 顶部刻度（左上角，带一点内边距）
                    drawList->AddText(ImVec2(plotPos.x + 3, plotPos.y + 1), axisColor, maxLabel);
                    // 底部刻度（左下角）
                    drawList->AddText(ImVec2(plotPos.x + 3, plotPos.y + plotSize.y - 13), axisColor, minLabel);

                    // 占位符以确保布局正确
                    ImGui::Dummy(plotSize);

                    // 自定义 tooltip
                    if (ImGui::IsItemHovered()) {
                        ImVec2 mousePos = ImGui::GetIO().MousePos;
                        float  relX     = (mousePos.x - plotPos.x) / plotSize.x;
                        float  logicalX = relX * (float)(historySize - 1) + scrollProgress;
                        int    idx      = (int)(logicalX + 0.5f);
                        if (idx >= 0 && idx < historySize) {
                            float fpsVal = getValue(idx);
                            ImGui::BeginTooltip();
                            ImGui::Text("%.0f FPS", fpsVal);
                            ImGui::EndTooltip();
                        }
                    }

                    ImGui::Dummy(ImVec2(0, 5));

                    // LOD 控制
                    MD3::Toggle(str.lodLock, &appState.lod.locked);
                    if (appState.lod.locked) {
                        ImGui::Indent(10);
                        // 手动调整粒子数（使用MD3::Slider）
                        float particleCountFloat = (float)appState.render.activeParticleCount;
                        if (MD3::Slider("##Particles", &particleCountFloat, (float)MIN_PARTICLES, (float)MAX_PARTICLES,
                                        "%.0f")) {
                            appState.render.activeParticleCount = (unsigned int)particleCountFloat;
                            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, particleBuffers.GetIndirectBuffer());
                            glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(unsigned int),
                                            &appState.render.activeParticleCount);
                        }
                        // 手动调整像素比例
                        MD3::Slider("##PixelRatio", &appState.render.pixelRatio, 0.5f, 1.0f, "%.2f");
                        ImGui::Unindent(10);
                    }

                    ImGui::Dummy(ImVec2(0, 5));

                    // VSync Mode selection
                    ImGui::Text("%s:", str.vsync);
                    int vsyncIndex;
                    if (appState.render.vsyncMode == 0) {
                        vsyncIndex = 0;
                    } else if (appState.render.vsyncMode == 1) {
                        vsyncIndex = 1;
                    } else {
                        vsyncIndex = 2; // -1 (Adaptive)
                    }

                    if (appState.render.adaptiveVSyncSupported) {
                        const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn, str.vsyncAdaptive};
                        if (MD3::Combo("##VSyncMode", &vsyncIndex, vsyncModes, 3)) {
                            int newMode               = (vsyncIndex == 0) ? 0 : (vsyncIndex == 1) ? 1 : -1;
                            appState.render.vsyncMode = newMode;
                            glfwSwapInterval(newMode);
                            std::cout << "[Main] VSync mode changed to: " << vsyncModes[vsyncIndex] << std::endl;
                        }
                    } else {
                        const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn};
                        if (MD3::Combo("##VSyncMode", &vsyncIndex, vsyncModes, 2)) {
                            appState.render.vsyncMode = vsyncIndex;
                            glfwSwapInterval(vsyncIndex);
                            std::cout << "[Main] VSync mode changed to: " << vsyncModes[vsyncIndex] << std::endl;
                        }
                    }
                    MD3::EndCollapsingHeader();
                }

                if (MD3::BeginCollapsingHeader(str.sectionHandTracking, true)) {
                    // ========== 追踪器状态卡 ==========
                    {
                        int         readyStatus = IsTrackerReady();
                        const char* statusText;
                        ImVec4      statusColor;

                        if (!handTrackerStarted) {
                            statusText  = str.trackerInitializing;
                            statusColor = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // 黄色
                        } else if (readyStatus == 1) {
                            statusText  = str.trackerReady;
                            statusColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // 绿色
                        } else if (readyStatus == -1) {
                            statusText  = str.trackerFailed;
                            statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // 红色
                        } else {
                            statusText  = str.trackerInitializing;
                            statusColor = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // 黄色
                        }

                        if (ImGui::BeginTable("TrackerStatusTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed,
                                                    100 * appState.ui.dpiScale);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                            // 追踪器状态
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", str.trackerStatus);
                            ImGui::TableNextColumn();
                            ImGui::TextColored(statusColor, "%s", statusText);

                            // 错误信息（如果有）
                            if (readyStatus == -1) {
                                int         errCode = GetTrackerLastError();
                                const char* localizedMsg;
                                switch (errCode) {
                                case HANDTRACKER_ERROR_PALM_MODEL:
                                    localizedMsg = str.palmModelLoadFailed;
                                    break;
                                case HANDTRACKER_ERROR_HAND_MODEL:
                                    localizedMsg = str.handModelLoadFailed;
                                    break;
                                case HANDTRACKER_ERROR_CAMERA_OPEN:
                                    localizedMsg = str.cameraInitFailed;
                                    break;
                                case HANDTRACKER_ERROR_CAMERA_IN_USE:
                                    localizedMsg = str.cameraInUse;
                                    break;
                                case HANDTRACKER_ERROR_NO_CAMERA:
                                    localizedMsg = str.cameraNotFound;
                                    break;
                                case HANDTRACKER_ERROR_THREAD:
                                    localizedMsg = str.unexpectedError;
                                    break;
                                default:
                                    localizedMsg = str.trackerFailed;
                                    break;
                                }
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextDisabled("%s", str.trackerError);
                                ImGui::TableNextColumn();
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", localizedMsg);
                            }

                            // 摄像头信息
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", str.cameraInfo);
                            ImGui::TableNextColumn();
                            if (readyStatus == 1) {
                                ImGui::Text("#0 (640x480)"); // TODO: 从 HandTracker 获取实际值
                            } else {
                                ImGui::TextDisabled("N/A");
                            }

                            // 手势检测
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", str.handDetected);
                            ImGui::TableNextColumn();
                            if (handState.hasHand) {
                                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", str.yes);
                            } else {
                                ImGui::Text("%s", str.no);
                            }

                            ImGui::EndTable();
                        }
                    }

                    ImGui::Separator();

                    // ========== 数值显示 ==========
                    if (ImGui::BeginTable("HandDataTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100 * appState.ui.dpiScale);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.scale);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", handState.scale);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Rot X");
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", handState.rotX);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Rot Y");
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", handState.rotY);

                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    ImGui::TextDisabled("%s:", str.animationScale);
                    if (ImGui::BeginTable("AnimTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100 * appState.ui.dpiScale);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.scale);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", currentAnim.scale);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.animationRotX);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", currentAnim.rotX);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.animationRotY);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", currentAnim.rotY);

                        ImGui::EndTable();
                    }

                    ImGui::Separator();

                    // ========== 可调参数 ==========
                    ImGui::Text("%s:", str.sensitivity);
                    MD3::Slider("##Sensitivity", &appState.handParams.sensitivity, 0.1f, 3.0f, "%.2f");

                    MD3::Toggle(str.invertX, &appState.handParams.invertX);
                    ImGui::SameLine(0, 20); // 增加间距
                    MD3::Toggle(str.invertY, &appState.handParams.invertY);

                    ImGui::Text("%s:", str.handLostDelay);
                    float handLostDelayFloat = (float)appState.handParams.handLostDelay;
                    if (MD3::Slider("##HandLostDelay", &handLostDelayFloat, 1.0f, 30.0f, "%.0f")) {
                        appState.handParams.handLostDelay = (int)handLostDelayFloat;
                    }

                    if (MD3::TonalButton(str.resetDefaults)) {
                        appState.handParams.sensitivity   = 1.0f;
                        appState.handParams.invertX       = false;
                        appState.handParams.invertY       = false;
                        appState.handParams.handLostDelay = 10;
                    }

                    ImGui::Separator();
                    bool cameraDebug = GetTrackerDebugMode();
                    if (MD3::Toggle(str.showCameraDebug, &cameraDebug)) {
                        SetTrackerDebugMode(cameraDebug);
                        appState.ui.showCameraDebug = cameraDebug;
                    }
                    MD3::EndCollapsingHeader();
                }

                if (MD3::BeginCollapsingHeader(str.sectionVisuals)) {
                    if (MD3::Toggle(str.darkMode, &appState.ui.isDarkMode)) {
                        UIManager::ApplyMaterialYouTheme(appState.ui.isDarkMode);
                        MD3::SetDarkMode(appState.ui.isDarkMode);
                    }
                    ImGui::Dummy(ImVec2(0, 5));
                    MD3::Toggle(str.glassBlur, &appState.ui.enableBlur);
                    if (appState.ui.enableBlur) {
                        ImGui::Indent(10);
                        MD3::Slider("##BlurStr", &appState.ui.blurStrength, 0.0f, 5.0f, "%.1f");
                        ImGui::Unindent(10);
                    }
                    MD3::EndCollapsingHeader();
                }

                if (MD3::BeginCollapsingHeader(str.sectionWindow)) {
#ifdef _WIN32
                    HWND hwnd             = glfwGetWin32Window(window);
                    auto GetBackdropLabel = [&](int mode) -> const char* {
                        switch (mode) {
                        case 0:
                            return str.backdropSolid;
                        case 1:
                            return str.backdropAero;
                        case 2:
                            return str.backdropAcrylic;
                        case 3:
                            return str.backdropMica;
                        default:
                            return str.statusUnknown;
                        }
                    };

                    std::vector<const char*> items;
                    items.reserve(appState.backdrop.availableBackdrops.size());
                    for (int mode : appState.backdrop.availableBackdrops) {
                        items.push_back(GetBackdropLabel(mode));
                    }

                    if (!items.empty()) {
                        ImGui::Text("%s:", str.backdrop);
                        int idx = appState.backdrop.backdropIndex;
                        if (idx < 0 || idx >= (int)items.size()) {
                            idx = 0;
                        }
                        if (MD3::Combo("##Backdrop", &idx, items.data(), (int)items.size())) {
                            appState.backdrop.backdropIndex = idx;
                            WindowManager::SetBackdropMode(hwnd, appState.backdrop.availableBackdrops[idx], appState);
                        }
                    }
#endif
                    ImGui::Text("%s: %s", str.fullscreen, appState.window.isFullscreen ? str.yes : str.no);
                    ImGui::Text("%s: %s", str.transparent, appState.backdrop.useTransparent ? str.yes : str.no);
                    MD3::EndCollapsingHeader();
                }

                if (MD3::BeginCollapsingHeader(str.sectionAdvanced)) {
                    // SIMD Mode selection
                    ImGui::Text("%s:", str.simdMode);
                    int         currentSIMD = GetTrackerSIMDMode();
                    const char* simdModes[] = {str.simdAuto, str.simdAVX2, str.simdSSE, str.simdScalar};
                    if (MD3::Combo("##SIMDMode", &currentSIMD, simdModes, 4)) {
                        SetTrackerSIMDMode(currentSIMD);
                        std::cout << "[Main] SIMD mode changed to: " << GetTrackerSIMDImplementation() << std::endl;
                    }
                    ImGui::Text("%s: %s", str.simdCurrent, GetTrackerSIMDImplementation());
                    MD3::EndCollapsingHeader();
                }

                if (MD3::BeginCollapsingHeader(str.sectionLog, true)) {
                    // 静态变量用于日志过滤
                    static char logSearchBuffer[128] = "";
                    static int  logLevelFilter       = 0; // 0=全部, 1=Info, 2=Warn, 3=Error

                    // 第一行：级别过滤、搜索和暂停按钮
                    float dpi           = appState.ui.dpiScale;
                    float controlHeight = 40.0f * dpi;   // MD3 标准控件高度（与 MD3::Combo 一致）
                    float buttonSize    = controlHeight; // 暂停按钮尺寸（正方形）

                    ImGui::SetNextItemWidth(80 * dpi);
                    const char* levelLabels[] = {str.logLevelAll, str.logLevelInfo, str.logLevelWarn,
                                                 str.logLevelError};
                    MD3::Combo("##LogLevel", &logLevelFilter, levelLabels, 4);

                    ImGui::SameLine();
                    // 搜索栏宽度 = 可用宽度 - 暂停按钮 - 间距 - 右侧留白
                    float itemSpacingX   = ImGui::GetStyle().ItemSpacing.x;
                    float rightMargin    = 12.0f * dpi; // 让暂停按钮不要贴右边界
                    float minSearchWidth = 120.0f * dpi;
                    float contentAvailX  = ImGui::GetContentRegionAvail().x;

                    float searchWidth = contentAvailX - buttonSize - itemSpacingX - rightMargin;
                    if (searchWidth < minSearchWidth) {
                        // 空间不足时优先保证输入框可用：取消右侧留白，避免宽度过小导致裁剪/交互异常
                        searchWidth = contentAvailX - buttonSize - itemSpacingX;
                    }
                    if (searchWidth < 1.0f) {
                        searchWidth = 1.0f;
                    }

                    ImGui::SetNextItemWidth(searchWidth);

                    // InputText 走 ImGui 自带绘制：通过 FramePadding 精确对齐 MD3 的 40dp
                    // 高度，并用圆角裁剪避免文字“顶出”圆角区域
                    float padY = (controlHeight - ImGui::GetFontSize()) * 0.5f;
                    if (padY < 0.0f) {
                        padY = 0.0f;
                    }

                    float inputRounding = controlHeight * 0.5f;
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f * dpi, padY));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, inputRounding);

                    ImVec2 inputPos  = ImGui::GetCursorScreenPos();
                    ImVec2 inputSize = ImVec2(searchWidth, controlHeight);
                    MD3::PushRoundedClipRect(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y),
                                             inputRounding);
                    ImGui::InputTextWithHint("##LogSearch", str.logSearch, logSearchBuffer, sizeof(logSearchBuffer));
                    MD3::PopRoundedClipRect();

                    ImGui::PopStyleVar(2);

                    // 右键粘贴菜单
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                    if (ImGui::BeginPopupContextItem("##LogSearchContext")) {
                        const char* clipText = ImGui::GetClipboardText();
                        bool        canPaste = (clipText && clipText[0] != '\0');
                        if (MD3::MenuItem(str.paste, canPaste, 36.0f * dpi)) {
                            // 追加粘贴内容到搜索栏
                            size_t currentLen = strlen(logSearchBuffer);
                            size_t clipLen    = strlen(clipText);
                            size_t maxAppend  = sizeof(logSearchBuffer) - 1 - currentLen;
                            if (clipLen > maxAppend) {
                                clipLen = maxAppend;
                            }
                            if (clipLen > 0) {
                                memcpy(logSearchBuffer + currentLen, clipText, clipLen);
                                logSearchBuffer[currentLen + clipLen] = '\0';
                            }
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleVar(2);

                    // 暂停/继续按钮（MD3风格按钮+图标）
                    ImGui::SameLine();
                    bool isPaused = DebugLog::Instance().IsPaused();

                    // 用 InvisibleButton 做交互热区，然后自绘 MD3 风格底 + 图标
                    ImGui::PushID("LogPauseBtn");
                    ImGui::InvisibleButton("##btn", ImVec2(buttonSize, buttonSize));

                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2      btnMin   = ImGui::GetItemRectMin();
                    ImVec2      btnMax   = ImGui::GetItemRectMax();
                    bool        hovered  = ImGui::IsItemHovered();
                    bool        held     = ImGui::IsItemActive();
                    bool        clicked  = ImGui::IsItemClicked(0);

                    const MD3::MD3ColorScheme colors =
                        MD3::IsDarkMode() ? MD3::GetDarkColorScheme() : MD3::GetLightColorScheme();
                    float rounding = std::min(12.0f * dpi, buttonSize * 0.5f);

                    if (clicked) {
                        MD3::TriggerRippleForCurrentItem(ImGui::GetItemID(), rounding);
                        DebugLog::Instance().SetPaused(!isPaused);
                    }

                    if (hovered) {
                        ImGui::SetTooltip("%s", isPaused ? str.logResume : str.logPause);
                    }

                    // 轻微阴影（更接近 MD3 elevation）
                    {
                        ImVec4 shadow = colors.shadow;
                        shadow.w      = 0.18f;
                        ImVec2 sMin(btnMin.x, btnMin.y + 1.0f * dpi);
                        ImVec2 sMax(btnMax.x, btnMax.y + 1.0f * dpi);
                        drawList->AddRectFilled(sMin, sMax, MD3::ColorToU32(shadow), rounding);
                    }

                    // 底色 + 状态层
                    ImVec4 bgColor = colors.surfaceContainerHigh;
                    if (hovered || held) {
                        float alpha = held ? colors.stateLayerPressed : colors.stateLayerHover;
                        bgColor     = MD3::ApplyStateLayer(bgColor, colors.onSurface, alpha);
                    }
                    drawList->AddRectFilled(btnMin, btnMax, MD3::ColorToU32(bgColor), rounding);

                    // 图标：使用离线烘焙的 alpha 掩码（由原 SVG 转换得到），运行时只上传一次纹理
                    float cx = (btnMin.x + btnMax.x) * 0.5f;
                    float cy = (btnMin.y + btnMax.y) * 0.5f;

                    float  drawIconPx = 24.0f * dpi;
                    GLuint texId      = GetOrCreateBakedLogIconTexture(isPaused);
                    if (texId != 0) {
                        ImVec2 iconMin(cx - drawIconPx * 0.5f, cy - drawIconPx * 0.5f);
                        ImVec2 iconMax(cx + drawIconPx * 0.5f, cy + drawIconPx * 0.5f);
                        drawList->AddImage((ImTextureID)(uintptr_t)texId, iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1),
                                           IM_COL32_WHITE);
                    }

                    ImGui::PopID();

                    // 第二行：清空和复制按钮
                    if (MD3::TonalButton(str.clearLog)) {
                        DebugLog::Instance().Clear();
                    }
                    ImGui::SameLine();
                    if (MD3::TonalButton(str.copyAllLog)) {
                        std::string filteredText =
                            DebugLog::Instance().GetFilteredText(logSearchBuffer, logLevelFilter);
                        ImGui::SetClipboardText(filteredText.c_str());
                    }

                    // 日志列表（带过滤和搜索）
                    DebugLog::Instance().Draw(logSearchBuffer, logLevelFilter);
                    MD3::EndCollapsingHeader();
                }

                // Crash Analyzer button
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                float buttonWidth  = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                float buttonHeight = 48 * appState.ui.dpiScale;
                if (MD3::FilledButton(str.cameraSelectorButton, ImVec2(buttonWidth, buttonHeight))) {
                    CameraSelector::ShowCameraSelectorDialog(glfwGetWin32Window(window), GetModuleHandle(nullptr),
                                                             true);
                }
                ImGui::SameLine();
                if (MD3::FilledButton(str.crashAnalyzerButton, ImVec2(buttonWidth, buttonHeight))) {
                    CrashAnalyzer::Open();
                }

                // 在窗口关闭前绘制 Ripple 效果（跟随滚动）
                MD3::DrawRipples();

                // 绘制 MD3 滚动条
                MD3::WindowScrollbar(40.0f * appState.ui.dpiScale);

                // 处理窗口 resize（自定义实现）
                MD3::WindowResize(300.0f * appState.ui.dpiScale, 200.0f * appState.ui.dpiScale);

                // 绘制标题栏（在所有内容之上）
                MD3::WindowTitleBar(str.debugPanelTitle, &appState.ui.showDebugWindow);

                ImGui::End();
            }

            // MD3 帧结束（必须在 ImGui::Render 之前）
            MD3::EndFrame();

            ImGui::Render();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        glfwSwapBuffers(window);
        glfwPollEvents();

        // Key handling (使用 AppState 中的输入状态)
        if (glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS) {
            if (!appState.input.keyF3_pressed) {
                appState.input.keyF3_pressed = true;
                appState.ui.showDebugWindow  = !appState.ui.showDebugWindow;

                // 惰性初始化 ImGui 和 MD3（首次打开调试窗口时）
                if (appState.ui.showDebugWindow && !appState.ui.imguiInitialized) {
                    std::cout << "[Main] Lazy-loading UI system..." << std::endl;
                    ErrorHandler::SetStage(ErrorHandler::AppStage::IMGUI_INIT);
                    UIManager::Init(window, appState);
                    MD3::Init(appState.ui.dpiScale);
                    MD3::SetDarkMode(appState.ui.isDarkMode);
                    MD3::SetScreenSize((float)appState.window.width, (float)appState.window.height);
                    appState.ui.imguiInitialized = true;
                    ErrorHandler::SetStage(ErrorHandler::AppStage::RENDER_LOOP);
                    std::cout << "[Main] UI system initialized (lazy load)" << std::endl;
                }

                std::cout << "[Main] Debug window: " << (appState.ui.showDebugWindow ? "shown" : "hidden") << std::endl;
            }
        } else {
            appState.input.keyF3_pressed = false;
        }

#ifdef _WIN32
        HWND hwnd = glfwGetWin32Window(window);
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS) {
            if (!appState.input.keyB_pressed) {
                appState.input.keyB_pressed = true;
                if (!appState.backdrop.availableBackdrops.empty()) {
                    appState.backdrop.backdropIndex =
                        (appState.backdrop.backdropIndex + 1) % (int)appState.backdrop.availableBackdrops.size();
                    WindowManager::SetBackdropMode(
                        hwnd, appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex], appState);
                }
            }
        } else {
            appState.input.keyB_pressed = false;
        }

        if (glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS) {
            if (!appState.input.keyF11_pressed) {
                appState.input.keyF11_pressed = true;
                WindowManager::ToggleFullscreen(window, appState);
                if (!appState.window.isFullscreen &&
                    appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex] > 0) {
                    WindowManager::SetBackdropMode(
                        hwnd, appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex], appState);
                }
            }
        } else {
            appState.input.keyF11_pressed = false;
        }
#endif

        if (glfwGetKey(window, GLFW_KEY_ESCAPE)) {
            break;
        }
    }

    // Cleanup
    // ErrorHandler::SetStage(ErrorHandler::AppStage::SHUTDOWN);
    std::cout << "[Main] Shutting down..." << std::endl;
    asyncTracker.Stop(); // 停止异步追踪线程
    CrashAnalyzer::Shutdown();
    if (appState.ui.imguiInitialized) {
        MD3::Shutdown();
        UIManager::Shutdown();
    }
    ReleaseTracker();
    glfwTerminate();
    return 0;
}
