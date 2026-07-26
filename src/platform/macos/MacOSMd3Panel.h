#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ParticleSaturn::App {
class AppController;
enum class WindowMaterial : unsigned char;
}

struct ImDrawList;
struct ImVec2;

namespace ParticleSaturn::Platform::MacOS {

struct Md3PanelCallbacks {
    std::function<void()> save;
    std::function<void()> toggleFullscreen;
    std::function<void()> showCameraSelector;
    std::function<void()> restartApplication;
    std::function<void(ParticleSaturn::App::WindowMaterial)> applyWindowMaterial;
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float rounding)> drawAcrylicBackground;
    // FPS 曲线等小区域的弱模糊背景；缺省时曲线退回纯色背景。
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float rounding)> drawGraphAcrylic;
};

struct Md3PanelHandTrackingStatus {
    enum class Tracker : std::uint8_t {
        Unavailable,
        Initializing,
        Ready,
        Failed,
    };

    Tracker tracker = Tracker::Unavailable;
    std::string errorMessage;
    std::string cameraInfo;
    bool handDetected = false;
    float rawScale = 1.0f;
    float rawRotX = 0.5f;
    float rawRotY = 0.625f;
};

void RenderMd3Panel(ParticleSaturn::App::AppController& controller, const char* backendName, std::uint32_t fps,
                    bool supportsAnalyticParticles, const Md3PanelCallbacks& callbacks,
                    const Md3PanelHandTrackingStatus& handStatus = {});

// 把 std::cout/std::cerr/std::clog 重定向进 MD3 调试日志（内容仍会写回
// 原始流）。幂等，进程内只安装一次。
void InstallDebugLogCapture();

} // namespace ParticleSaturn::Platform::MacOS
