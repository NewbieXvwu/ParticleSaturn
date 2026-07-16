#pragma once

#include <algorithm>
#include <cstdint>

namespace ParticleSaturn::App {

enum class GraphicsApi : std::uint8_t {
    OpenGL41,
    Vulkan,
    Metal,
};

enum class VulkanDriver : std::uint8_t {
    MoltenVK,
    KosmicKrisp,
};

enum class WindowMaterial : std::uint8_t {
    Solid,
    Transparent,
    SystemBlur,
    AppAcrylic,
};

struct SceneState {
    double simulationTimeSeconds = 0.0;
    // 与旧 OpenGL/Diligent 渲染器的初始姿态一致。
    float rotationX              = 0.4f;
    float rotationY              = 0.0f;
    float zoom                   = 1.0f;
    float autoAnimationTime      = 0.0f;
    std::uint32_t randomSeed     = 0x53415455U;
    bool paused                   = false;
};

struct RenderSettings {
    static constexpr std::uint32_t MinParticles = 200000;
    static constexpr std::uint32_t MaxParticles = 1200000;

    std::uint32_t particleCount = MaxParticles;
    float pixelRatio             = 1.0f;
    float densityCompensation    = 0.6f;
    int vsyncMode                = -1;
    bool bloomEnabled            = true;
    GraphicsApi graphicsApi      = GraphicsApi::Vulkan;
    VulkanDriver vulkanDriver    = VulkanDriver::MoltenVK;
};

struct UiState {
    bool showDebugWindow = false;
    bool showCameraDebug = false;
    bool darkMode        = true;
    bool blurEnabled     = true;
    float blurStrength   = 2.0f;
    float noiseIntensity = 0.01f;
};

struct GestureSettings {
    float sensitivity   = 1.0f;
    bool invertX        = false;
    bool invertY        = false;
    int handLostDelay   = 10;
};

struct WindowState {
    std::uint32_t width  = 1920;
    std::uint32_t height = 1080;
    float dpiScale       = 1.0f;
    bool fullscreen      = false;
    WindowMaterial material = WindowMaterial::Solid;
};

struct AppState {
    SceneState scene;
    RenderSettings render;
    UiState ui;
    GestureSettings gesture;
    WindowState window;
};

inline float Clamp(float value, float minimum, float maximum) {
    return std::clamp(value, minimum, maximum);
}

} // namespace ParticleSaturn::App
