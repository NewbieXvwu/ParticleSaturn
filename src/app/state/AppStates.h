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
    bool analyticParticles       = false;
    // Bloom is a scene post-process.  Keep its blur radius independent from
    // the Acrylic control, which only affects ImGui window backgrounds.
    float bloomBlurStrength      = 2.0f;
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

struct LodState {
    bool locked = false;
    float smoothedFrameSeconds = 1.0f / 60.0f;
};

struct InputState {
    bool keyBPressed = false;
    bool keyF3Pressed = false;
    bool keyF11Pressed = false;
    bool keyEscapePressed = false;
};

struct WindowState {
    std::int32_t x = 100;
    std::int32_t y = 100;
    std::uint32_t width  = 1920;
    std::uint32_t height = 1080;
    std::int32_t windowedX = 100;
    std::int32_t windowedY = 100;
    std::uint32_t windowedWidth = 1920;
    std::uint32_t windowedHeight = 1080;
    float dpiScale       = 1.0f;
    bool fullscreen      = false;
    WindowMaterial material = WindowMaterial::Solid;
};

struct AppState {
    SceneState scene;
    RenderSettings render;
    UiState ui;
    GestureSettings gesture;
    LodState lod;
    InputState input;
    WindowState window;
};

inline float Clamp(float value, float minimum, float maximum) {
    return std::clamp(value, minimum, maximum);
}

} // namespace ParticleSaturn::App
