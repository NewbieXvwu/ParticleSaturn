#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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
    bool useObjectShader         = false;
    // Bloom is a scene post-process.  Keep its blur radius independent from
    // the Acrylic control, which only affects ImGui window backgrounds.
    float bloomBlurStrength      = 2.0f;
    GraphicsApi graphicsApi      = GraphicsApi::Vulkan;
    VulkanDriver vulkanDriver    = VulkanDriver::MoltenVK;
    bool adaptiveVSyncSupported  = false;
};

struct UiState {
    bool showDebugWindow = false;
    bool showCameraDebug = false;
    bool darkMode        = true;
    bool blurEnabled     = true;
    float blurStrength   = 2.0f;
    float noiseIntensity = 0.01f;
    bool imguiInitialized = false;
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
    int lastDecision = 0;
    float lastDecisionTime = 0.0f;
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
    bool resized         = true;
    WindowMaterial material = WindowMaterial::Solid;
};

// Windows DWM 背景材质选择状态（macOS 以 WindowState::material 表达，Windows
// 侧沿用可用材质列表 + 当前索引的原生形态）。
struct BackdropState {
    std::vector<int> availableBackdrops = {0};
    int backdropIndex = 0;
    bool useTransparent = false;
    bool transparentSupported = true;
};

// OpenGL 驱动信息（崩溃报告用）。
struct GlInfo {
    std::string version;
    std::string renderer;
    int major = 0;
    int minor = 0;
    bool persistentMapping = false;
};

struct AppState {
    SceneState scene;
    RenderSettings render;
    UiState ui;
    GestureSettings gesture;
    LodState lod;
    InputState input;
    WindowState window;
    BackdropState backdrop;
    GlInfo gl;
};

inline float Clamp(float value, float minimum, float maximum) {
    return std::clamp(value, minimum, maximum);
}

} // namespace ParticleSaturn::App
