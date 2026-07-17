#pragma once

#include "app/state/AppStates.h"

#include <cstdint>
#include <functional>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLBloom;
class OpenGLParticleSystem;
class OpenGLRenderTargets;
class OpenGLSevenSegmentFps;
class OpenGLStarField;
class OpenGLToneMapper;

struct OpenGLFrameCallbacks {
    std::function<bool(std::uint32_t, std::uint32_t, std::uint32_t)> capture;
    std::function<bool(std::uint32_t, std::uint32_t)> renderUi;
    std::function<void()> present;
};

class OpenGLFrameRenderer {
public:
    bool Render(OpenGLParticleSystem& particles, OpenGLStarField& stars, OpenGLRenderTargets& targets,
                OpenGLBloom& bloom, OpenGLToneMapper& toneMapper, OpenGLSevenSegmentFps& sevenSegment,
                std::uint32_t width, std::uint32_t height, const App::AppState& state, bool handTracked,
                float deltaTime, std::uint32_t framesPerSecond, bool transparent,
                const OpenGLFrameCallbacks& callbacks = {});
};

} // namespace ParticleSaturn::Gpu::OpenGL41
