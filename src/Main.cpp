// Particle Saturn - 土星粒子系统
// GPU 粒子计算 + 动态 LOD + 手势追踪 + 实时渲染

#include "pch.h"

#ifdef EMBED_MODELS
#include "Resource.h"
#endif

#include "AppState.h"
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

// 初始窗口尺寸常量
const unsigned int INIT_WIDTH  = 1920;
const unsigned int INIT_HEIGHT = 1080;

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

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 4); // 升级到 4.4 以支持 glBufferStorage
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    // 创建窗口
    GLFWwindow* window = glfwCreateWindow(INIT_WIDTH, INIT_HEIGHT, "Particle Saturn", NULL, NULL);
    if (!window) {
        std::cerr << "[Main] Fatal: glfwCreateWindow() failed" << std::endl;
        ErrorHandler::ShowEarlyFatalError(
            i18n::Get().windowCreateFailed,
            "glfwCreateWindow() returned NULL.\n\n"
            "This usually means your GPU does not support OpenGL 4.4 Core Profile.\n"
            "Please update your graphics driver or check hardware compatibility.");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);

    // 加载 OpenGL 扩展
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "[Main] Fatal: gladLoadGLLoader() failed" << std::endl;
        ErrorHandler::ShowEarlyFatalError(
            i18n::Get().openglLoadFailed,
            "gladLoadGLLoader() returned false.\n\n"
            "Failed to load OpenGL function pointers.\n"
            "Please update your graphics driver.");
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    // 验证 OpenGL 版本
    int glMajor, glMinor;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);
    if (glMajor < 4 || (glMajor == 4 && glMinor < 4)) {
        std::cerr << "[Main] Fatal: OpenGL " << glMajor << "." << glMinor << " < 4.4" << std::endl;
        std::ostringstream details;
        details << "Detected OpenGL version: " << glMajor << "." << glMinor << "\n"
                << "Required: OpenGL 4.4 or higher\n\n"
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

    // Store OpenGL info for crash reports
    ErrorHandler::SetStage(ErrorHandler::AppStage::OPENGL_INIT);
    appState.gl.version  = (const char*)glGetString(GL_VERSION);
    appState.gl.renderer = (const char*)glGetString(GL_RENDERER);
    ErrorHandler::SetGPUInfo(appState.gl.renderer, appState.gl.version);
    std::cout << "[Main] OpenGL: " << appState.gl.version << std::endl;

#ifdef _WIN32
    ImmAssociateContext(glfwGetWin32Window(window), NULL);

    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd) {
        WindowManager::SetTitleBarDarkMode(hwnd, true);
        appState.ui.isDarkMode = WindowManager::IsSystemDarkMode();
        std::cout << "[DWM] System theme: " << (appState.ui.isDarkMode ? "Dark" : "Light") << std::endl;
        WindowManager::InstallThemeChangeHook(hwnd);
        WindowManager::DetectAvailableBackdrops(hwnd, appState);
        appState.backdrop.backdropIndex = (int)appState.backdrop.availableBackdrops.size() - 1;
        WindowManager::SetBackdropMode(hwnd, appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex],
                                       appState);
    }
#endif

    // 初始化手部追踪
    ErrorHandler::SetStage(ErrorHandler::AppStage::HAND_TRACKER_INIT);
    bool handTrackerInitialized = false;
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
    } else {
        HGLOBAL hPalmData = LoadResource(NULL, hPalmRes);
        HGLOBAL hHandData = LoadResource(NULL, hHandRes);
        if (!hPalmData || !hHandData) {
            std::cerr << "[Main] Warning: Failed to load embedded model resources" << std::endl;
            ErrorHandler::ShowWarning(i18n::Get().embeddedResourceFailed, "LoadResource() failed");
        } else {
            const void* palmData = LockResource(hPalmData);
            const void* handData = LockResource(hHandData);
            DWORD       palmSize = SizeofResource(NULL, hPalmRes);
            DWORD       handSize = SizeofResource(NULL, hHandRes);
            SetEmbeddedModels(palmData, palmSize, handData, handSize);
            std::cout << "[Main] Embedded models loaded (palm: " << palmSize << " bytes, hand: " << handSize << " bytes)"
                      << std::endl;
        }
    }
    if (!InitTracker(0, nullptr)) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker" << std::endl;
        ErrorHandler::ShowWarning(i18n::Get().cameraInitFailed, "InitTracker() returned false (embedded models)");
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
        handTrackerInitialized = true;
        ErrorHandler::SetCameraInfo(0, 640, 480, true);
    }
#else
    std::cout << "[Main] Initializing HandTracker..." << std::endl;
    if (!InitTracker(0, ".")) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker DLL." << std::endl;
        ErrorHandler::ShowWarning(i18n::Get().cameraInitFailed, "InitTracker() returned false");
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
        handTrackerInitialized = true;
        ErrorHandler::SetCameraInfo(0, 640, 480, true);
    }
#endif

    ErrorHandler::SetStage(ErrorHandler::AppStage::IMGUI_INIT);
    UIManager::Init(window, appState);

    // 创建着色器程序
    ErrorHandler::SetStage(ErrorHandler::AppStage::SHADER_COMPILE);
    unsigned int pSaturn = Renderer::CreateProgram(Shaders::VertexSaturn, Shaders::FragmentSaturn);
    unsigned int pStar   = Renderer::CreateProgram(Shaders::VertexStar, Shaders::FragmentStar);
    unsigned int pPlanet = Renderer::CreateProgram(Shaders::VertexPlanet, Shaders::FragmentPlanet);
    unsigned int pUI     = Renderer::CreateProgram(Shaders::VertexUI, Shaders::FragmentUI);
    unsigned int pQuad   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentQuad);
    unsigned int pBlur   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentBlur);

    // 检查核心着色器是否编译成功
    if (!pSaturn || !pStar || !pPlanet || !pUI || !pQuad || !pBlur) {
        std::cerr << "[Main] Fatal: Core shader compilation failed" << std::endl;
        std::ostringstream details;
        details << "Shader compilation status:\n"
                << "  pSaturn: " << (pSaturn ? "OK" : "FAILED") << "\n"
                << "  pStar:   " << (pStar ? "OK" : "FAILED") << "\n"
                << "  pPlanet: " << (pPlanet ? "OK" : "FAILED") << "\n"
                << "  pUI:     " << (pUI ? "OK" : "FAILED") << "\n"
                << "  pQuad:   " << (pQuad ? "OK" : "FAILED") << "\n"
                << "  pBlur:   " << (pBlur ? "OK" : "FAILED") << "\n\n"
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
    auto resizeFBO = [&](int width, int height) {
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    resizeFBO(appState.window.width, appState.window.height);

    // 模糊效果 FBO
    BlurFramebuffer fboBlur1, fboBlur2;
    fboBlur1.Init(appState.window.width / 6, appState.window.height / 6);
    fboBlur2.Init(appState.window.width / 6, appState.window.height / 6);

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
        std::cerr << "Failed to initialize particle system" << std::endl;
        ErrorHandler::ShowError(i18n::Get().shaderCompileFailed, "ParticleSystem::InitParticlesGPU() returned false");
        glfwTerminate();
        return -1;
    }

    // 创建星空背景
    unsigned int vaoStars, vboStars;
    ParticleSystem::CreateStars(vaoStars, vboStars);

    // 创建行星网格
    unsigned int vaoPlanet, idxPlanet;
    Renderer::CreateSphere(vaoPlanet, idxPlanet, 1.0f);

    // 生成 FBM 噪声纹理 (预计算替代程序化噪声)
    unsigned int fbmTexture = Renderer::GenerateFBMTexture();

    // 使用预定义的行星常量数据
    const auto& planets     = PlanetConstants::kPlanets;
    const int   planetCount = PlanetConstants::kPlanetCount;

    // 预生成数字几何 (FPS 显示优化)
    Renderer::PrebuiltDigits prebuiltDigits;
    prebuiltDigits.Init();

    glEnable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glDepthMask(GL_FALSE);

    // 初始化 Uniform 缓存
    UniformCache uc;
    Renderer::InitUniformCache(uc, pComp, pSaturn, pStar, pPlanet, pUI, pBlur, pQuad);

    // 投影和视图矩阵
    glm::mat4 proj   = glm::perspective(1.047f, (float)appState.window.width / appState.window.height, 1.f, 10000.f);
    glm::mat4 view   = glm::lookAt(glm::vec3(0, 0, 100), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 projUI = glm::ortho(0.0f, (float)appState.window.width, 0.0f, (float)appState.window.height);

    // 动画状态
    SmoothState currentAnim;
    float       autoTime = 0;

    // 异步手部追踪器 (优化: 消除主线程阻塞)
    AsyncHandTracker asyncTracker;
    if (handTrackerInitialized) {
        asyncTracker.Start();
    }

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

        // 处理窗口大小变化
        if (appState.window.resized) {
            appState.window.resized = false;
            proj   = glm::perspective(1.047f, (float)appState.window.width / appState.window.height, 1.f, 10000.f);
            projUI = glm::ortho(0.0f, (float)appState.window.width, 0.0f, (float)appState.window.height);
            resizeFBO(appState.window.width, appState.window.height);
            fboBlur1.Init(appState.window.width / 6, appState.window.height / 6);
            fboBlur2.Init(appState.window.width / 6, appState.window.height / 6);
        }

        // 获取手部追踪数据 (异步: 非阻塞读取最新状态)
        HandState handState = asyncTracker.GetLatestState();

        // 优化: 使用环形缓冲区计算平滑 FPS
        fpsCalculator.AddFrameTime(dt);
        currentFps = fpsCalculator.GetAverageFPS();

        // 动态 LOD 调整 (每 0.5 秒检查一次)
        lodUpdateTimer += dt;
        if (lodUpdateTimer >= 0.5f) {
            lodUpdateTimer = 0.0f;

            float smoothedFps          = currentFps; // 环形缓冲区已经提供平滑值
            bool  particleCountChanged = false;
            bool  pixelRatioChanged    = false;

            // 扩展滞后区间: 38-57 FPS 进一步减少边界震荡
            if (smoothedFps < 38.0f) {
                // 更保守的降质策略: 0.95 替代 0.9
                if (appState.render.activeParticleCount > MIN_PARTICLES) {
                    appState.render.activeParticleCount = (unsigned int)(appState.render.activeParticleCount * 0.95f);
                    particleCountChanged                = true;
                } else if (appState.render.pixelRatio > 0.7f) {
                    appState.render.pixelRatio -= 0.03f;
                    pixelRatioChanged = true;
                }
            } else if (smoothedFps > 57.0f) {
                // 更保守的提质策略: 1.05 替代 1.1
                if (appState.render.pixelRatio < 1.0f) {
                    appState.render.pixelRatio += 0.03f;
                    pixelRatioChanged = true;
                } else if (appState.render.activeParticleCount < MAX_PARTICLES) {
                    appState.render.activeParticleCount = (unsigned int)(appState.render.activeParticleCount * 1.05f);
                    particleCountChanged                = true;
                }
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
        }

        // 动画逻辑
        float targetScale, targetRotX, targetRotY;
        if (!handState.hasHand) {
            autoTime += 0.005f;
            targetScale       = 1.0f + sin(autoTime) * 0.2f;
            targetRotX        = 0.4f + sin(autoTime * 0.3f) * 0.15f;
            targetRotY        = 0.0f;
            float lerpFactor  = 0.08f;
            currentAnim.scale = Lerp(currentAnim.scale, targetScale, lerpFactor);
            currentAnim.rotX  = Lerp(currentAnim.rotX, targetRotX, lerpFactor);
            currentAnim.rotY  = Lerp(currentAnim.rotY, targetRotY, lerpFactor);
        } else {
            targetScale = handState.scale;
            targetRotX  = -0.6f + handState.rotY * 1.6f;
            targetRotY  = (handState.rotX - 0.5f) * 2.0f;
            // 使用插值平滑过渡，避免 30fps 摄像头数据在 90fps 渲染时的跳变
            float lerpFactor  = 0.25f;
            currentAnim.scale = Lerp(currentAnim.scale, targetScale, lerpFactor);
            currentAnim.rotX  = Lerp(currentAnim.rotX, targetRotX, lerpFactor);
            currentAnim.rotY  = Lerp(currentAnim.rotY, targetRotY, lerpFactor);
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

        // 渲染行星 (实例化渲染优化 - 单次 draw call)
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(pPlanet);
        glUniformMatrix4fv(uc.pl_p, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.pl_v, 1, 0, &view[0][0]);
        glUniform3f(uc.pl_ld, 1, .5, 1);
        glUniform1i(uc.pl_uPlanetCount, planetCount);
        // 绑定预计算的 FBM 噪声纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fbmTexture);
        glUniform1i(uc.pl_uFBMTex, 0);

        // 更新行星 UBO 数据 (直接写入 persistent mapped buffer，无需 glBufferSubData)
        glm::mat4 orbitRot = glm::rotate(glm::mat4(1.f), t * 0.02f, glm::vec3(0, 1, 0));
        float     selfRot  = t * 0.1f;

        // 直接写入 persistent mapped buffer (无 CPU-GPU 同步开销)
        for (int i = 0; i < planetCount; i++) {
            const PlanetData& p             = planets[i];
            glm::mat4         m             = orbitRot;
            m                               = glm::translate(m, p.pos);
            m                               = glm::rotate(m, selfRot, glm::vec3(0, 1, 0));
            m                               = glm::scale(m, glm::vec3(p.radius));
            uc.pl_ubo_mapped[i].modelMatrix = m;
            uc.pl_ubo_mapped[i].color1      = glm::vec4(p.color1, p.noiseScale);
            uc.pl_ubo_mapped[i].color2      = glm::vec4(p.color2, p.atmosphere);
        }

        // 渲染所有行星 (GL_MAP_COHERENT_BIT 保证自动同步)
        glBindVertexArray(vaoPlanet);
        glDrawElementsInstanced(GL_TRIANGLES, idxPlanet, GL_UNSIGNED_INT, 0, planetCount);

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
        GLuint finalBlurTex = fboBlur2.tex; // 最终模糊结果纹理
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
        glClear(GL_COLOR_BUFFER_BIT);

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

        // 渲染 ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render error dialogs
        ErrorHandler::RenderErrorDialog(dt);

        // Render crash analyzer window
        CrashAnalyzer::Render(appState.ui.enableBlur, fboBlur2.tex, appState.window.width, appState.window.height,
                              appState.ui.isDarkMode);

        if (appState.ui.showDebugWindow) {
            const auto& str = i18n::Get();
            ImGui::SetNextWindowSize(ImVec2(450 * appState.ui.dpiScale, 600 * appState.ui.dpiScale),
                                     ImGuiCond_FirstUseEver);
            ImGuiStyle& style            = ImGui::GetStyle();
            ImVec4      originalWindowBg = style.Colors[ImGuiCol_WindowBg];

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
            ImGui::Begin(str.debugPanelTitle, &appState.ui.showDebugWindow, ImGuiWindowFlags_NoCollapse);

            ImVec2      pos  = ImGui::GetWindowPos();
            ImVec2      size = ImGui::GetWindowSize();
            ImDrawList* dl   = ImGui::GetWindowDrawList();

            if (appState.ui.enableBlur) {
                ImVec2 uv0 = ImVec2(pos.x / appState.window.width, 1.0f - pos.y / appState.window.height);
                ImVec2 uv1 =
                    ImVec2((pos.x + size.x) / appState.window.width, 1.0f - (pos.y + size.y) / appState.window.height);
                dl->AddImage((ImTextureID)(intptr_t)fboBlur2.tex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0,
                             uv1);
                ImU32 tintColor = appState.ui.isDarkMode ? IM_COL32(20, 20, 25, 180) : IM_COL32(245, 245, 255, 150);
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tintColor, style.WindowRounding);
                ImU32 highlight = appState.ui.isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
            } else {
                // Use the saved original background color instead of the overridden transparent one
                ImVec4 bgCol = originalWindowBg;
                bgCol.w      = 0.95f;
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol),
                                  style.WindowRounding);
            }
            ImGui::PopStyleColor(4);

            if (ImGui::CollapsingHeader(str.sectionPerformance, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("%s: %.1f", str.fps, currentFps);
                ImGui::Text("%s: %u / %u", str.particles, appState.render.activeParticleCount, MAX_PARTICLES);
                ImGui::Text("%s: %.2f", str.pixelRatio, appState.render.pixelRatio);
                ImGui::Text("%s: %u x %u", str.resolution, appState.window.width, appState.window.height);

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

                ImGui::SetNextItemWidth(-1);
                if (appState.render.adaptiveVSyncSupported) {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn, str.vsyncAdaptive};
                    if (ImGui::Combo("##VSyncMode", &vsyncIndex, vsyncModes, 3)) {
                        int newMode               = (vsyncIndex == 0) ? 0 : (vsyncIndex == 1) ? 1 : -1;
                        appState.render.vsyncMode = newMode;
                        glfwSwapInterval(newMode);
                        std::cout << "[Main] VSync mode changed to: " << vsyncModes[vsyncIndex] << std::endl;
                    }
                } else {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn};
                    if (ImGui::Combo("##VSyncMode", &vsyncIndex, vsyncModes, 2)) {
                        appState.render.vsyncMode = vsyncIndex;
                        glfwSwapInterval(vsyncIndex);
                        std::cout << "[Main] VSync mode changed to: " << vsyncModes[vsyncIndex] << std::endl;
                    }
                }
            }

            if (ImGui::CollapsingHeader(str.sectionHandTracking, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("%s: %s", str.handDetected, handState.hasHand ? str.yes : str.no);
                ImGui::Text("%s: %.3f", str.scale, handState.scale);
                ImGui::Text("Rot X: %.3f", handState.rotX);
                ImGui::Text("Rot Y: %.3f", handState.rotY);
                ImGui::Separator();
                ImGui::Text("%s: %.3f", str.animationScale, currentAnim.scale);
                ImGui::Text("%s: %.3f", str.animationRotX, currentAnim.rotX);
                ImGui::Text("%s: %.3f", str.animationRotY, currentAnim.rotY);
                ImGui::Separator();
                bool cameraDebug = GetTrackerDebugMode();
                if (UIManager::ToggleMD3(str.showCameraDebug, &cameraDebug, dt)) {
                    SetTrackerDebugMode(cameraDebug);
                    appState.ui.showCameraDebug = cameraDebug;
                }
            }

            if (ImGui::CollapsingHeader(str.sectionVisuals)) {
                if (UIManager::ToggleMD3(str.darkMode, &appState.ui.isDarkMode, dt)) {
                    UIManager::ApplyMaterialYouTheme(appState.ui.isDarkMode);
                }
                ImGui::Dummy(ImVec2(0, 5));
                UIManager::ToggleMD3(str.glassBlur, &appState.ui.enableBlur, dt);
                if (appState.ui.enableBlur) {
                    ImGui::Indent(10);
                    ImGui::SetNextItemWidth(-1);
                    char blurLabel[64];
                    snprintf(blurLabel, sizeof(blurLabel), "%s: %%.0f", str.blurStrength);
                    ImGui::SliderFloat("##BlurStr", &appState.ui.blurStrength, 0.0f, 5.0f, blurLabel);
                    ImGui::Unindent(10);
                }
            }

            if (ImGui::CollapsingHeader(str.sectionWindow)) {
                const char* backdropNames[] = {"Solid Black", "Acrylic", "Mica"};
                if (appState.backdrop.backdropIndex < (int)appState.backdrop.availableBackdrops.size()) {
                    ImGui::Text("%s: %s", str.backdrop,
                                backdropNames[appState.backdrop.availableBackdrops[appState.backdrop.backdropIndex]]);
                }
                ImGui::Text("%s: %s", str.fullscreen, appState.window.isFullscreen ? str.yes : str.no);
                ImGui::Text("%s: %s", str.transparent, appState.backdrop.useTransparent ? str.yes : str.no);
            }

            if (ImGui::CollapsingHeader(str.sectionAdvanced)) {
                // SIMD Mode selection
                ImGui::Text("%s:", str.simdMode);
                int         currentSIMD = GetTrackerSIMDMode();
                const char* simdModes[] = {str.simdAuto, str.simdAVX2, str.simdSSE, str.simdScalar};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##SIMDMode", &currentSIMD, simdModes, 4)) {
                    SetTrackerSIMDMode(currentSIMD);
                    std::cout << "[Main] SIMD mode changed to: " << GetTrackerSIMDImplementation() << std::endl;
                }
                ImGui::Text("%s: %s", str.simdCurrent, GetTrackerSIMDImplementation());
            }

            if (ImGui::CollapsingHeader(str.sectionLog, ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button(str.clearLog)) {
                    DebugLog::Instance().Clear();
                }
                ImGui::SameLine();
                if (ImGui::Button(str.copyAllLog)) {
                    std::string allText = DebugLog::Instance().GetAllText();
                    ImGui::SetClipboardText(allText.c_str());
                }
                DebugLog::Instance().Draw();
            }

            // Crash Analyzer button
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button(str.crashAnalyzerButton, ImVec2(-1, 36 * appState.ui.dpiScale))) {
                CrashAnalyzer::Open();
            }

            ImGui::End();
        }

        ImGui::Render();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();

        // Key handling (使用 AppState 中的输入状态)
        if (glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS) {
            if (!appState.input.keyF3_pressed) {
                appState.input.keyF3_pressed = true;
                appState.ui.showDebugWindow  = !appState.ui.showDebugWindow;
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
    UIManager::Shutdown();
    ReleaseTracker();
    glfwTerminate();
    return 0;
}
