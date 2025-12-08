// Particle Saturn - 土星粒子系统
// GPU 粒子计算 + 动态 LOD + 手势追踪 + 实时渲染

#include "pch.h"

#ifdef EMBED_MODELS
#include "Resource.h"
#endif

#include "DebugLog.h"
#include "HandTracker.h"
#include "ParticleSystem.h"
#include "Renderer.h"
#include "Shaders.h"
#include "UIManager.h"
#include "Utils.h"
#include "WindowManager.h"

// 全局状态
const unsigned int INIT_WIDTH  = 1920;
const unsigned int INIT_HEIGHT = 1080;

unsigned int g_scrWidth      = INIT_WIDTH;
unsigned int g_scrHeight     = INIT_HEIGHT;
bool         g_windowResized = true;

std::vector<int> g_availableBackdrops = {0};
int              g_backdropIndex      = 0;
bool             g_useTransparent     = false;

bool g_isFullscreen = false;
int  g_windowedX = 100, g_windowedY = 100;
int  g_windowedW = INIT_WIDTH, g_windowedH = INIT_HEIGHT;

bool  g_showDebugWindow = false;
bool  g_showCameraDebug = false;
float g_dpiScale        = 1.0f;
bool  g_isDarkMode      = true;

bool  g_enableImGuiBlur = true;
float g_blurStrength    = 2.0f;

unsigned int g_activeParticleCount = MAX_PARTICLES;
float        g_currentPixelRatio   = 1.0f;

std::map<ImGuiID, UIAnimState> g_animStates;

int main() {
    // 重定向 cout 到调试日志
    static DebugStreamBuf debugBuf(std::cout.rdbuf());
    std::cout.rdbuf(&debugBuf);

    std::cout << "[Main] Particle Saturn starting..." << std::endl;

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(INIT_WIDTH, INIT_HEIGHT, "Particle Saturn", NULL, NULL);
    glfwMakeContextCurrent(window);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glfwSetFramebufferSizeCallback(window, WindowManager::FramebufferSizeCallback);

#ifdef _WIN32
    ImmAssociateContext(glfwGetWin32Window(window), NULL);

    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd) {
        WindowManager::SetTitleBarDarkMode(hwnd, true);
        g_isDarkMode = WindowManager::IsSystemDarkMode();
        std::cout << "[DWM] System theme: " << (g_isDarkMode ? "Dark" : "Light") << std::endl;
        WindowManager::InstallThemeChangeHook(hwnd);
        WindowManager::DetectAvailableBackdrops(hwnd);
        g_backdropIndex = (int)g_availableBackdrops.size() - 1;
        WindowManager::SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
    }
#endif

    // 初始化手部追踪
#ifdef EMBED_MODELS
    std::cout << "[Main] Loading embedded models..." << std::endl;
    HRSRC hPalmRes = FindResource(NULL, MAKEINTRESOURCE(IDR_PALM_MODEL), RT_RCDATA);
    HRSRC hHandRes = FindResource(NULL, MAKEINTRESOURCE(IDR_HAND_MODEL), RT_RCDATA);
    if (hPalmRes && hHandRes) {
        HGLOBAL hPalmData = LoadResource(NULL, hPalmRes);
        HGLOBAL hHandData = LoadResource(NULL, hHandRes);
        if (hPalmData && hHandData) {
            const void* palmData = LockResource(hPalmData);
            const void* handData = LockResource(hHandData);
            DWORD       palmSize = SizeofResource(NULL, hPalmRes);
            DWORD       handSize = SizeofResource(NULL, hHandRes);
            SetEmbeddedModels(palmData, palmSize, handData, handSize);
            std::cout << "[Main] Embedded models loaded" << std::endl;
        }
    }
    if (!InitTracker(0, nullptr)) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker" << std::endl;
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
    }
#else
    std::cout << "[Main] Initializing HandTracker..." << std::endl;
    if (!InitTracker(0, ".")) {
        std::cerr << "[Main] Warning: Failed to initialize HandTracker DLL." << std::endl;
    } else {
        std::cout << "[Main] HandTracker initialized successfully." << std::endl;
    }
#endif

    UIManager::Init(window);

    // 创建着色器程序
    unsigned int pSaturn = Renderer::CreateProgram(Shaders::VertexSaturn, Shaders::FragmentSaturn);
    unsigned int pStar   = Renderer::CreateProgram(Shaders::VertexStar, Shaders::FragmentStar);
    unsigned int pPlanet = Renderer::CreateProgram(Shaders::VertexPlanet, Shaders::FragmentPlanet);
    unsigned int pUI     = Renderer::CreateProgram(Shaders::VertexUI, Shaders::FragmentUI);
    unsigned int pQuad   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentQuad);
    unsigned int pBlur   = Renderer::CreateProgram(Shaders::VertexQuad, Shaders::FragmentBlur);

    unsigned int cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &Shaders::ComputeSaturn, 0);
    glCompileShader(cs);
    unsigned int pComp = glCreateProgram();
    glAttachShader(pComp, cs);
    glLinkProgram(pComp);

    // 离屏渲染 FBO
    unsigned int fbo, fboTex, rbo;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &fboTex);
    glGenRenderbuffers(1, &rbo);

    auto resizeFBO = [&](int width, int height) {
        glBindTexture(GL_TEXTURE_2D, fboTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    };
    resizeFBO(g_scrWidth, g_scrHeight);

    // 模糊效果 FBO
    BlurFramebuffer fboBlur1, fboBlur2;
    fboBlur1.Init(g_scrWidth / 6, g_scrHeight / 6);
    fboBlur2.Init(g_scrWidth / 6, g_scrHeight / 6);

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

    // 初始化粒子系统
    std::vector<GPUParticle> particles;
    ParticleSystem::InitParticles(particles);
    unsigned int ssbo, vaoParticles;
    ParticleSystem::CreateBuffers(ssbo, vaoParticles, particles);

    // 创建星空背景
    unsigned int vaoStars, vboStars;
    ParticleSystem::CreateStars(vaoStars, vboStars);

    // 创建行星网格
    unsigned int vaoPlanet, idxPlanet;
    Renderer::CreateSphere(vaoPlanet, idxPlanet, 1.0f);

    // UI 渲染 VAO
    unsigned int vaoUI, vboUI;
    glGenVertexArrays(1, &vaoUI);
    glGenBuffers(1, &vboUI);
    glBindVertexArray(vaoUI);
    glBindBuffer(GL_ARRAY_BUFFER, vboUI);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0);
    glBindVertexArray(0);

    glEnable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glDepthMask(GL_FALSE);

    // 初始化 Uniform 缓存
    UniformCache uc;
    Renderer::InitUniformCache(uc, pComp, pSaturn, pStar, pPlanet, pUI);

    // 投影和视图矩阵
    glm::mat4 proj   = glm::perspective(1.047f, (float)g_scrWidth / g_scrHeight, 1.f, 10000.f);
    glm::mat4 view   = glm::lookAt(glm::vec3(0, 0, 100), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 projUI = glm::ortho(0.0f, (float)g_scrWidth, 0.0f, (float)g_scrHeight);

    // UI 顶点缓冲
    std::vector<float> uiVerts;
    uiVerts.reserve(256);

    // 动画状态
    SmoothState currentAnim;
    HandState   handState;
    float       autoTime = 0;

    // 主循环变量
    float lastFrame        = 0;
    int   frameCount       = 0;
    float lastFpsTime      = 0;
    float currentFps       = 60.0f;
    int   lastDisplayedFps = 60;

    // 主渲染循环
    while (!glfwWindowShouldClose(window)) {
        float t   = (float)glfwGetTime();
        float dt  = t - lastFrame;
        lastFrame = t;

        // 处理窗口大小变化
        if (g_windowResized) {
            g_windowResized  = false;
            proj             = glm::perspective(1.047f, (float)g_scrWidth / g_scrHeight, 1.f, 10000.f);
            projUI           = glm::ortho(0.0f, (float)g_scrWidth, 0.0f, (float)g_scrHeight);
            lastDisplayedFps = -1;
            resizeFBO(g_scrWidth, g_scrHeight);
            fboBlur1.Init(g_scrWidth / 6, g_scrHeight / 6);
            fboBlur2.Init(g_scrWidth / 6, g_scrHeight / 6);
        }

        // 获取手部追踪数据
        GetHandData(&handState.scale, &handState.rotX, &handState.rotY, &handState.hasHand);

        // 动态 LOD 调整
        frameCount++;
        if (t - lastFpsTime >= 0.5f) {
            currentFps  = frameCount / (t - lastFpsTime);
            frameCount  = 0;
            lastFpsTime = t;
            if (currentFps < 45.0f) {
                if (g_activeParticleCount > MIN_PARTICLES) {
                    g_activeParticleCount = (unsigned int)(g_activeParticleCount * 0.9f);
                } else if (g_currentPixelRatio > 0.7f) {
                    g_currentPixelRatio -= 0.05f;
                }
            } else if (currentFps > 58.0f) {
                if (g_currentPixelRatio < 1.0f) {
                    g_currentPixelRatio += 0.05f;
                } else if (g_activeParticleCount < MAX_PARTICLES) {
                    g_activeParticleCount = (unsigned int)(g_activeParticleCount * 1.1f);
                }
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
            targetScale       = handState.scale;
            targetRotX        = -0.6f + handState.rotY * 1.6f;
            targetRotY        = (handState.rotX - 0.5f) * 2.0f;
            currentAnim.scale = targetScale;
            currentAnim.rotX  = targetRotX;
            currentAnim.rotY  = targetRotY;
        }

        // 计算粒子物理
        glUseProgram(pComp);
        glUniform1f(uc.comp_uDt, dt);
        glUniform1f(uc.comp_uHandScale, currentAnim.scale);
        glUniform1f(uc.comp_uHandHas, handState.hasHand ? 1.0f : 0.0f);
        glUniform1ui(uc.comp_uParticleCount, g_activeParticleCount);
        glDispatchCompute((g_activeParticleCount + 255) / 256, 1, 1);
        glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);

        // 渲染到 FBO
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);

        glm::mat4 mSat = glm::mat4(1.f);
        mSat           = glm::rotate(mSat, currentAnim.rotX, glm::vec3(1, 0, 0));
        mSat           = glm::rotate(mSat, currentAnim.rotY, glm::vec3(0, 1, 0));
        mSat           = glm::rotate(mSat, 0.466f, glm::vec3(0, 0, 1));

        // 渲染星空
        glUseProgram(pStar);
        glUniformMatrix4fv(uc.star_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.star_view, 1, 0, &view[0][0]);
        glm::mat4 mStar = glm::rotate(glm::mat4(1.f), t * 0.005f, glm::vec3(0, 1, 0));
        glUniformMatrix4fv(uc.star_model, 1, 0, &mStar[0][0]);
        glUniform1f(uc.star_uTime, t);
        glBindVertexArray(vaoStars);
        glDrawArrays(GL_POINTS, 0, 50000);

        // 渲染土星粒子
        glUseProgram(pSaturn);
        glUniformMatrix4fv(uc.sat_proj, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.sat_view, 1, 0, &view[0][0]);
        glUniformMatrix4fv(uc.sat_model, 1, 0, &mSat[0][0]);
        glUniform1f(uc.sat_uTime, t);
        glUniform1f(uc.sat_uScale, currentAnim.scale);
        glUniform1f(uc.sat_uPixelRatio, g_currentPixelRatio);
        float ratio       = (float)g_activeParticleCount / MAX_PARTICLES;
        float densityComp = 0.6f / pow(ratio, 0.7f) / pow(g_currentPixelRatio, 0.5f);
        glUniform1f(uc.sat_uDensityComp, densityComp);
        glUniform1f(uc.sat_uScreenHeight, (float)g_scrHeight);
        glBindVertexArray(vaoParticles);
        glDrawArrays(GL_POINTS, 0, g_activeParticleCount);

        // 渲染行星
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(pPlanet);
        glUniformMatrix4fv(uc.pl_p, 1, 0, &proj[0][0]);
        glUniformMatrix4fv(uc.pl_v, 1, 0, &view[0][0]);
        glUniform3f(uc.pl_ld, 1, .5, 1);
        glBindVertexArray(vaoPlanet);

        auto drawPlanet = [&](glm::vec3 pos, float r, glm::vec3 c1, glm::vec3 c2, float ns, float at) {
            glm::mat4 m = glm::rotate(glm::mat4(1.f), t * 0.02f, glm::vec3(0, 1, 0));
            m           = glm::translate(m, pos);
            m           = glm::rotate(m, t * 0.1f, glm::vec3(0, 1, 0));
            m           = glm::scale(m, glm::vec3(r));
            glUniformMatrix4fv(uc.pl_m, 1, 0, &m[0][0]);
            glUniform3f(uc.pl_c1, c1.x, c1.y, c1.z);
            glUniform3f(uc.pl_c2, c2.x, c2.y, c2.z);
            glUniform1f(uc.pl_ns, ns);
            glUniform1f(uc.pl_at, at);
            glDrawElements(GL_TRIANGLES, idxPlanet, GL_UNSIGNED_INT, 0);
        };
        drawPlanet({-300, 120, -450}, 10, HexToRGB(0xb33a00), HexToRGB(0xd16830), 8, .3f);
        drawPlanet({380, -100, -600}, 14, HexToRGB(0x001e4d), HexToRGB(0xffffff), 5, .6f);
        drawPlanet({-180, -220, -350}, 6, HexToRGB(0x666666), HexToRGB(0xaaaaaa), 15, .1f);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        // 渲染 FPS 显示
        int displayFps = (int)currentFps;
        if (displayFps != lastDisplayedFps) {
            lastDisplayedFps = displayFps;
            uiVerts.clear();
            std::string fpsStr  = std::to_string(displayFps);
            float       xCursor = (float)g_scrWidth - 60.0f;
            float       numSize = 20.0f;
            for (int i = (int)fpsStr.length() - 1; i >= 0; i--) {
                Renderer::AddDigitGeometry(uiVerts, xCursor, (float)g_scrHeight - 40, numSize, numSize * 1.8f,
                                           fpsStr[i] - '0');
                xCursor -= (numSize + 10.0f);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vboUI);
            glBufferData(GL_ARRAY_BUFFER, uiVerts.size() * sizeof(float), uiVerts.data(), GL_DYNAMIC_DRAW);
        }
        glUseProgram(pUI);
        glUniformMatrix4fv(uc.ui_proj, 1, 0, &projUI[0][0]);
        glm::vec3 fpsCol = (currentFps > 50)
                             ? glm::vec3(0.3, 1.0, 0.3)
                             : ((currentFps > 30) ? glm::vec3(1.0, 0.6, 0.0) : glm::vec3(1.0, 0.2, 0.2));
        glUniform3fv(uc.ui_uColor, 1, &fpsCol[0]);
        glBindVertexArray(vaoUI);
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, (GLsizei)(uiVerts.size() / 2));

        // 模糊处理
        if (g_enableImGuiBlur) {
            glBlendFunc(GL_ONE, GL_ZERO);
            glViewport(0, 0, fboBlur1.w, fboBlur1.h);
            glUseProgram(pBlur);
            GLint locDir = glGetUniformLocation(pBlur, "dir");
            GLint locTex = glGetUniformLocation(pBlur, "uTexture");
            glUniform1i(locTex, 0);
            glActiveTexture(GL_TEXTURE0);

            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
            glBindTexture(GL_TEXTURE_2D, fboTex);
            glUniform2f(locDir, 1.0f / fboBlur1.w, 0.0f);
            glBindVertexArray(vaoQuad);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glBindFramebuffer(GL_FRAMEBUFFER, fboBlur2.fbo);
            glBindTexture(GL_TEXTURE_2D, fboBlur1.tex);
            glUniform2f(locDir, 0.0f, 1.0f / fboBlur1.h);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            int iterations = (int)g_blurStrength;
            for (int i = 0; i < iterations; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fboBlur1.fbo);
                glBindTexture(GL_TEXTURE_2D, fboBlur2.tex);
                glUniform2f(locDir, 1.0f / fboBlur1.w, 0.0f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                glBindFramebuffer(GL_FRAMEBUFFER, fboBlur2.fbo);
                glBindTexture(GL_TEXTURE_2D, fboBlur1.tex);
                glUniform2f(locDir, 0.0f, 1.0f / fboBlur1.h);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            glViewport(0, 0, g_scrWidth, g_scrHeight);
        }

        // 合成到屏幕
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (g_useTransparent) {
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
        glUniform1i(glGetUniformLocation(pQuad, "uTexture"), 0);
        glUniform1i(glGetUniformLocation(pQuad, "uTransparent"), g_useTransparent ? 1 : 0);
        glBindVertexArray(vaoQuad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 渲染 ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (g_showDebugWindow) {
            ImGui::SetNextWindowSize(ImVec2(450 * g_dpiScale, 600 * g_dpiScale), ImGuiCond_FirstUseEver);
            ImGuiStyle& style            = ImGui::GetStyle();
            ImVec4      originalWindowBg = style.Colors[ImGuiCol_WindowBg];

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
            ImGui::Begin("Debug Panel", &g_showDebugWindow, ImGuiWindowFlags_NoCollapse);

            ImVec2      pos  = ImGui::GetWindowPos();
            ImVec2      size = ImGui::GetWindowSize();
            ImDrawList* dl   = ImGui::GetWindowDrawList();

            if (g_enableImGuiBlur) {
                ImVec2 uv0 = ImVec2(pos.x / g_scrWidth, 1.0f - pos.y / g_scrHeight);
                ImVec2 uv1 = ImVec2((pos.x + size.x) / g_scrWidth, 1.0f - (pos.y + size.y) / g_scrHeight);
                dl->AddImage((ImTextureID)(intptr_t)fboBlur2.tex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0,
                             uv1);
                ImU32 tintColor = g_isDarkMode ? IM_COL32(20, 20, 25, 180) : IM_COL32(245, 245, 255, 150);
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tintColor, style.WindowRounding);
                ImU32 highlight = g_isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
            } else {
                // Use the saved original background color instead of the overridden transparent one
                ImVec4 bgCol = originalWindowBg;
                bgCol.w      = 0.95f;
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol),
                                  style.WindowRounding);
            }
            ImGui::PopStyleColor(4);

            if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("FPS: %.1f", currentFps);
                ImGui::Text("Particles: %u / %u", g_activeParticleCount, MAX_PARTICLES);
                ImGui::Text("Pixel Ratio: %.2f", g_currentPixelRatio);
                ImGui::Text("Resolution: %u x %u", g_scrWidth, g_scrHeight);
            }

            if (ImGui::CollapsingHeader("Hand Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Hand Detected: %s", handState.hasHand ? "Yes" : "No");
                ImGui::Text("Scale: %.3f", handState.scale);
                ImGui::Text("Rot X: %.3f", handState.rotX);
                ImGui::Text("Rot Y: %.3f", handState.rotY);
                ImGui::Separator();
                ImGui::Text("Animation Scale: %.3f", currentAnim.scale);
                ImGui::Text("Animation RotX: %.3f", currentAnim.rotX);
                ImGui::Text("Animation RotY: %.3f", currentAnim.rotY);
                ImGui::Separator();
                bool cameraDebug = GetTrackerDebugMode();
                if (UIManager::ToggleMD3("Show Camera Debug Window", &cameraDebug, dt)) {
                    SetTrackerDebugMode(cameraDebug);
                    g_showCameraDebug = cameraDebug;
                }
            }

            if (ImGui::CollapsingHeader("Visuals")) {
                if (UIManager::ToggleMD3("Dark Mode", &g_isDarkMode, dt)) {
                    UIManager::ApplyMaterialYouTheme(g_isDarkMode);
                }
                ImGui::Dummy(ImVec2(0, 5));
                UIManager::ToggleMD3("Glass Blur", &g_enableImGuiBlur, dt);
                if (g_enableImGuiBlur) {
                    ImGui::Indent(10);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##BlurStr", &g_blurStrength, 0.0f, 5.0f, "Blur Strength: %.0f");
                    ImGui::Unindent(10);
                }
            }

            if (ImGui::CollapsingHeader("Window")) {
                const char* backdropNames[] = {"Solid Black", "Acrylic", "Mica"};
                if (g_backdropIndex < (int)g_availableBackdrops.size()) {
                    ImGui::Text("Backdrop: %s", backdropNames[g_availableBackdrops[g_backdropIndex]]);
                }
                ImGui::Text("Fullscreen: %s", g_isFullscreen ? "Yes" : "No");
                ImGui::Text("Transparent: %s", g_useTransparent ? "Yes" : "No");
            }

            if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button("Clear")) {
                    DebugLog::Instance().Clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Copy All")) {
                    std::string allText = DebugLog::Instance().GetAllText();
                    ImGui::SetClipboardText(allText.c_str());
                }
                DebugLog::Instance().Draw();
            }

            ImGui::End();
        }

        ImGui::Render();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();

        // Key handling
        static bool keyB_pressed = false, keyF11_pressed = false, keyF3_pressed = false;

        if (glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS) {
            if (!keyF3_pressed) {
                keyF3_pressed     = true;
                g_showDebugWindow = !g_showDebugWindow;
                std::cout << "[Main] Debug window: " << (g_showDebugWindow ? "shown" : "hidden") << std::endl;
            }
        } else {
            keyF3_pressed = false;
        }

#ifdef _WIN32
        HWND hwnd = glfwGetWin32Window(window);
        if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS) {
            if (!keyB_pressed) {
                keyB_pressed = true;
                if (!g_availableBackdrops.empty()) {
                    g_backdropIndex = (g_backdropIndex + 1) % (int)g_availableBackdrops.size();
                    WindowManager::SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
                }
            }
        } else {
            keyB_pressed = false;
        }

        if (glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS) {
            if (!keyF11_pressed) {
                keyF11_pressed = true;
                WindowManager::ToggleFullscreen(window);
                if (!g_isFullscreen && g_availableBackdrops[g_backdropIndex] > 0) {
                    WindowManager::SetBackdropMode(hwnd, g_availableBackdrops[g_backdropIndex]);
                }
            }
        } else {
            keyF11_pressed = false;
        }
#endif

        if (glfwGetKey(window, GLFW_KEY_ESCAPE)) {
            break;
        }
    }

    // Cleanup
    std::cout << "[Main] Shutting down..." << std::endl;
    UIManager::Shutdown();
    ReleaseTracker();
    glfwTerminate();
    return 0;
}
