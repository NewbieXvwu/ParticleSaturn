#pragma once
// AppState - 应用程序全局状态封装
// 替代原有的全局变量，提供更好的代码组织和可维护性

#include <string>
#include <vector>
#include <unordered_map>

// 前向声明
struct GLFWwindow;

// 应用程序状态结构体
struct AppState {
    // 窗口状态
    struct {
        unsigned int width = 1920;
        unsigned int height = 1080;
        bool resized = true;
        bool isFullscreen = false;
        int windowedX = 100;
        int windowedY = 100;
        int windowedW = 1920;
        int windowedH = 1080;
    } window;

    // 渲染状态
    struct {
        unsigned int activeParticleCount = 0;  // 将在初始化时设置为 MAX_PARTICLES
        float pixelRatio = 1.0f;
        float densityComp = 0.6f;  // 缓存的密度补偿值
    } render;

    // UI 状态
    struct {
        bool showDebugWindow = false;
        bool showCameraDebug = false;
        float dpiScale = 1.0f;
        bool isDarkMode = true;
        bool enableBlur = true;
        float blurStrength = 2.0f;
    } ui;

    // 输入状态 (按键防抖)
    struct {
        bool keyB_pressed = false;
        bool keyF3_pressed = false;
        bool keyF11_pressed = false;
    } input;

    // 背景效果状态 (Windows DWM)
    struct {
        std::vector<int> availableBackdrops = {0};
        int backdropIndex = 0;
        bool useTransparent = false;
    } backdrop;

    // OpenGL 信息 (用于崩溃报告)
    struct {
        std::string version;
        std::string renderer;
    } gl;

    // 初始化默认值
    void InitDefaults(unsigned int maxParticles) {
        render.activeParticleCount = maxParticles;
    }
};

// 从 GLFWwindow 获取 AppState 指针的辅助函数
AppState* GetAppState(GLFWwindow* window);

// 设置 AppState 到 GLFWwindow 的辅助函数
void SetAppState(GLFWwindow* window, AppState* state);
