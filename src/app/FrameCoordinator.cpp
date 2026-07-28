#include "FrameCoordinator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ParticleSaturn::App {

FrameCoordinator::FrameCoordinator(double fixedStepSeconds) : fixedStepSeconds_{fixedStepSeconds} {
    if (fixedStepSeconds_ <= 0.0) {
        throw std::invalid_argument{"fixedStepSeconds must be positive"};
    }
}

FrameSnapshot FrameCoordinator::Advance(AppController& controller, double elapsedSeconds, const GestureInput& gesture) {
    const double clampedElapsed = std::clamp(elapsedSeconds, 0.0, 0.25);
    accumulator_ += clampedElapsed;
    auto& state = controller.MutableState();
    UpdateLod(state, clampedElapsed);
    while (accumulator_ >= fixedStepSeconds_) {
        Update(state, gesture);
        accumulator_ -= fixedStepSeconds_;
    }
    ++frameIndex_;
    return {frameIndex_, static_cast<float>(accumulator_ / fixedStepSeconds_), &state};
}

void FrameCoordinator::UpdateLod(AppState& state, double elapsedSeconds) {
    if (elapsedSeconds <= 0.0) {
        return;
    }
    const float frameSeconds = static_cast<float>(elapsedSeconds);
    state.lod.smoothedFrameSeconds += (frameSeconds - state.lod.smoothedFrameSeconds) * 0.1f;
    if (state.lod.locked) {
        return;
    }

    constexpr float SlowFrameSeconds = 1.0f / 50.0f;
    constexpr float FastFrameSeconds = 1.0f / 75.0f;
    if (state.lod.smoothedFrameSeconds > SlowFrameSeconds) {
        if (state.render.particleCount > RenderSettings::MinParticles) {
            state.render.particleCount =
                std::max(RenderSettings::MinParticles,
                         state.render.particleCount - std::max(10'000U, state.render.particleCount / 10U));
        } else {
            state.render.pixelRatio = std::max(0.25f, state.render.pixelRatio - 0.05f);
        }
    } else if (state.lod.smoothedFrameSeconds < FastFrameSeconds) {
        if (state.render.pixelRatio < 1.0f) {
            state.render.pixelRatio = std::min(1.0f, state.render.pixelRatio + 0.05f);
        } else {
            state.render.particleCount =
                std::min(RenderSettings::MaxParticles,
                         state.render.particleCount + std::max(10'000U, state.render.particleCount / 20U));
        }
    }
}

void FrameCoordinator::Update(AppState& state, const GestureInput& gesture) {
    if (state.scene.paused) {
        return;
    }
    state.scene.simulationTimeSeconds += fixedStepSeconds_;
    if (!gesture.tracked) {
        // 严格保持旧 OpenGL/Diligent 的时间尺度：0.005 * 180 每秒。
        state.scene.autoAnimationTime += static_cast<float>(fixedStepSeconds_ * (0.005 * 180.0));
        const float targetZoom      = 1.0f + std::sin(state.scene.autoAnimationTime) * 0.2f;
        const float targetRotationX = 0.4f + std::sin(state.scene.autoAnimationTime * 0.3f) * 0.15f;
        const float alpha           = 1.0f - std::pow(1.0f - 0.08f, static_cast<float>(fixedStepSeconds_ * 180.0));
        state.scene.zoom += (targetZoom - state.scene.zoom) * alpha;
        state.scene.rotationX += (targetRotationX - state.scene.rotationX) * alpha;
        state.scene.rotationY += (0.0f - state.scene.rotationY) * alpha;
        return;
    }

    if (gesture.hasAbsolutePose) {
        const float rotX01 = state.gesture.invertX ? 1.0f - gesture.rotationXNormalized : gesture.rotationXNormalized;
        const float rotY01 = state.gesture.invertY ? 1.0f - gesture.rotationYNormalized : gesture.rotationYNormalized;
        const float sensitivity     = Clamp(state.gesture.sensitivity, 0.1f, 3.0f);
        const float targetZoom      = gesture.scale;
        const float targetRotationX = (-0.6f + rotY01 * 1.6f) * sensitivity;
        const float targetRotationY = ((rotX01 - 0.5f) * 2.0f) * sensitivity;
        const float alpha           = 1.0f - std::pow(1.0f - 0.25f, static_cast<float>(fixedStepSeconds_ * 180.0));
        state.scene.zoom += (targetZoom - state.scene.zoom) * alpha;
        state.scene.rotationX += (targetRotationX - state.scene.rotationX) * alpha;
        state.scene.rotationY += (targetRotationY - state.scene.rotationY) * alpha;
        return;
    }

    const float xSign = state.gesture.invertX ? -1.0f : 1.0f;
    const float ySign = state.gesture.invertY ? -1.0f : 1.0f;
    state.scene.rotationX += gesture.rotationDeltaX * state.gesture.sensitivity * xSign;
    state.scene.rotationY += gesture.rotationDeltaY * state.gesture.sensitivity * ySign;
    state.scene.zoom = Clamp(state.scene.zoom + gesture.zoomDelta * state.gesture.sensitivity, 0.1f, 10.0f);
}

} // namespace ParticleSaturn::App
