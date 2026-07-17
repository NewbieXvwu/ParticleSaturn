#pragma once

#include <cstdint>
#include <functional>

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
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&)> drawAcrylicBackground;
};

void RenderMd3Panel(ParticleSaturn::App::AppController& controller, const char* backendName, std::uint32_t fps,
                    bool supportsAnalyticParticles, const Md3PanelCallbacks& callbacks);

} // namespace ParticleSaturn::Platform::MacOS
