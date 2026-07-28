#include "SmokeHarness.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "services/diagnostics/DiagnosticBus.h"

namespace ParticleSaturn::Platform::MacOS {

namespace {

std::uint32_t SmokeFramesFromEnvironment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return 0;
    }
    const auto parsed = std::strtoul(value, nullptr, 10);
    return static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 100UL));
}

} // namespace

SmokeConfig SmokeConfig::FromEnvironment() {
    SmokeConfig config;
    config.baselinePath      = std::getenv("PARTICLESATURN_CAPTURE_BASELINE");
    config.captureBaseline   = config.baselinePath != nullptr && config.baselinePath[0] != '\0';
    config.performanceFrames = SmokeFramesFromEnvironment("PARTICLESATURN_PERFORMANCE_LOCK_SMOKE");
    config.performanceSmoke  = config.performanceFrames != 0;
    config.fullscreenFrames  = SmokeFramesFromEnvironment("PARTICLESATURN_FULLSCREEN_RESTORE_SMOKE");
    config.fullscreenSmoke   = config.fullscreenFrames != 0;
    return config;
}

void SmokeConfig::ForceInitialState(App::AppState& state) const {
    if (captureBaseline) {
        state.window.width  = 1512;
        state.window.height = 827;
        state.scene.paused  = true;
        state.lod.locked    = true;
    }
    if (performanceSmoke) {
        state.render.particleCount     = App::RenderSettings::MaxParticles;
        state.render.pixelRatio        = 1.0f;
        state.render.bloomEnabled      = true;
        state.render.bloomBlurStrength = 2.0f;
        state.ui.blurEnabled           = true;
        state.ui.blurStrength          = 2.0f;
        state.lod.locked               = true;
    }
    if (fullscreenSmoke) {
        state.window.x              = 24;
        state.window.y              = 36;
        state.window.width          = 1111;
        state.window.height         = 777;
        state.window.windowedX      = 100;
        state.window.windowedY      = 100;
        state.window.windowedWidth  = 640;
        state.window.windowedHeight = 360;
        state.window.fullscreen     = true;
    }
}

StartupGeometry ResolveStartupGeometry(const App::AppState& state) {
    StartupGeometry geometry;
    geometry.restoreFullscreen = state.window.fullscreen;
    geometry.width             = geometry.restoreFullscreen ? state.window.windowedWidth : state.window.width;
    geometry.height            = geometry.restoreFullscreen ? state.window.windowedHeight : state.window.height;
    geometry.x                 = geometry.restoreFullscreen ? state.window.windowedX : state.window.x;
    geometry.y                 = geometry.restoreFullscreen ? state.window.windowedY : state.window.y;
    return geometry;
}

SmokeHarness::SmokeHarness(const SmokeConfig& config, const StartupGeometry& startup, std::string backendDomain,
                           HostOps ops)
    : config_{config},
      startup_{startup},
      backendDomain_{std::move(backendDomain)},
      ops_{std::move(ops)},
      fullscreenDeadline_{std::chrono::steady_clock::now() + std::chrono::seconds{5}} {}

void SmokeHarness::Fail(bool& flag, const char* code, const char* what) {
    std::string message = "[smoke] FAILED: " + backendDomain_ + " " + what;
    std::fprintf(stderr, "%s\n", message.c_str());
    Services::Diagnostics::DiagnosticBus::Instance().Publish(backendDomain_, code, message,
                                                             Services::Diagnostics::Severity::Error);
    flag = true;
    if (ops_.requestExit) {
        ops_.requestExit();
    }
}

void SmokeHarness::TickPerformance(const App::AppState& state) {
    if (!config_.performanceSmoke || performanceFailed_) {
        return;
    }
    if (state.render.particleCount != App::RenderSettings::MaxParticles || state.render.pixelRatio != 1.0f ||
        !state.render.bloomEnabled || state.render.bloomBlurStrength != 2.0f || !state.ui.blurEnabled ||
        state.ui.blurStrength != 2.0f || !state.lod.locked) {
        Fail(performanceFailed_, "performance-lock-smoke", "quality lock state changed during performance smoke test");
    } else if (++performanceFrameCount_ >= config_.performanceFrames) {
        if (ops_.requestExit) {
            ops_.requestExit();
        }
    }
}

void SmokeHarness::TickFullscreen(bool nativeFullscreen, const App::AppState& state, std::uint32_t logicalWidth,
                                  std::uint32_t logicalHeight, std::int32_t windowX, std::int32_t windowY) {
    if (!config_.fullscreenSmoke || fullscreenFailed_) {
        return;
    }
    if (nativeFullscreen && !fullscreenExitRequested_) {
        if (++fullscreenFrameCount_ >= config_.fullscreenFrames) {
            if (ops_.toggleFullscreen) {
                ops_.toggleFullscreen();
            }
            fullscreenExitRequested_ = true;
            fullscreenFrameCount_    = 0;
            fullscreenDeadline_      = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        }
    } else if (!nativeFullscreen && fullscreenExitRequested_) {
        const bool geometryRestored = logicalWidth == startup_.width && logicalHeight == startup_.height &&
                                      windowX == startup_.x && windowY == startup_.y;
        if (geometryRestored && !state.window.fullscreen) {
            if (++fullscreenFrameCount_ >= config_.fullscreenFrames && ops_.requestExit) {
                ops_.requestExit();
            }
        } else if (std::chrono::steady_clock::now() >= fullscreenDeadline_) {
            Fail(fullscreenFailed_, "fullscreen-restore-smoke", "window geometry was not restored after fullscreen");
        }
    } else if (std::chrono::steady_clock::now() >= fullscreenDeadline_) {
        Fail(fullscreenFailed_, "fullscreen-restore-smoke", "window did not complete fullscreen transition");
    }
}

} // namespace ParticleSaturn::Platform::MacOS
