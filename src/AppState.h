#pragma once
// AppState - 应用程序全局状态封装（旧模型）
//
// 【冻结声明 · D-002/D-005 · AUDIT P1-4】
// 本头是 Windows 专属的遗留全局状态模型，仅被 src/OpenGL 与 src/Diligent 两个
// Windows 目标消费（GetAppState/SetAppState 经 GLFWwindow 用户指针存取）。
// 它**有意不迁移**到 macOS 的重设计 `src/app/state/AppStates.h`
// （`ParticleSaturn::App::AppState`）：本模型携带 macOS 模型刻意舍弃的 Windows 专属
// 状态——DWM 背景材质选择（backdrop）、OpenGL 崩溃报告信息（gl）、GLFW 窗口指针
// 辅助函数、imgui 惰性初始化标志、按键防抖输入、LOD 决策码——两模型是不同设计而非
// 变体。Windows 目标处于冻结区，此处保持原样；新代码请勿把两模型混用或强行合并。

#include <string>
#include <unordered_map>
#include <vector>

// 前向声明
struct GLFWwindow;

// 应用程序状态结构体
struct AppState {
    // 窗口状态
    struct {
        unsigned int width        = 1920;
        unsigned int height       = 1080;
        bool         resized      = true;
        bool         isFullscreen = false;
        int          windowedX    = 100;
        int          windowedY    = 100;
        int          windowedW    = 1920;
        int          windowedH    = 1080;
    } window;

    // 渲染状态
    struct {
        unsigned int activeParticleCount    = 0; // 将在初始化时设置为 MAX_PARTICLES
        float        pixelRatio             = 1.0f;
        float        densityComp            = 0.6f; // 缓存的密度补偿值
        int          vsyncMode              = -1;   // -1: Adaptive, 0: Off, 1: On
        bool         adaptiveVSyncSupported = false;
    } render;

    // UI 状态
    struct {
        bool  showDebugWindow = false;
        bool  showCameraDebug = false;
        float dpiScale        = 1.0f;
        bool  isDarkMode      = true;
        bool  enableBlur      = true;
        float blurStrength    = 2.0f;
        // Acrylic 噪点强度：用于防 banding，目标应“几乎不可见”
        // 建议范围：0.0 ~ 0.03（默认 0.01 左右）
        float noiseIntensity   = 0.01f;
        bool  imguiInitialized = false; // 惰性加载标志
    } ui;

    // 手势追踪参数 (用户可调)
    struct {
        float sensitivity   = 1.0f;  // 灵敏度倍数
        bool  invertX       = false; // 反转 X 轴
        bool  invertY       = false; // 反转 Y 轴
        int   handLostDelay = 10;    // 丢手延迟帧数
    } handParams;

    // LOD 控制状态
    struct {
        bool  locked           = false; // 锁定 LOD，禁用自动调整
        int   lastDecision     = 0;     // 0=稳定, 1=降粒子, 2=降像素, 3=升像素, 4=升粒子
        float lastDecisionTime = 0.0f;  // 上次决策时间
    } lod;

    // 输入状态 (按键防抖)
    struct {
        bool keyB_pressed   = false;
        bool keyF3_pressed  = false;
        bool keyF11_pressed = false;
    } input;

    // 背景效果状态 (Windows DWM)
    struct {
        std::vector<int> availableBackdrops   = {0};
        int              backdropIndex        = 0;
        bool             useTransparent       = false;
        bool             transparentSupported = true; // 系统是否支持透明效果 (Win10 1809+)
    } backdrop;

    // OpenGL 信息 (用于崩溃报告)
    struct {
        std::string version;
        std::string renderer;
        int         major             = 0;
        int         minor             = 0;
        bool        persistentMapping = false; // OpenGL 4.4+ 特性
    } gl;

    // 初始化默认值
    void InitDefaults(unsigned int maxParticles) { render.activeParticleCount = maxParticles; }
};

// 从 GLFWwindow 获取 AppState 指针的辅助函数
AppState* GetAppState(GLFWwindow* window);

// 设置 AppState 到 GLFWwindow 的辅助函数
void SetAppState(GLFWwindow* window, AppState* state);
