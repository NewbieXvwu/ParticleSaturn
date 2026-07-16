#include "FrameCoordinator.h"

#include <algorithm>
#include <stdexcept>

namespace ParticleSaturn::App {

FrameCoordinator::FrameCoordinator(double fixedStepSeconds) : fixedStepSeconds_{fixedStepSeconds} {
    if (fixedStepSeconds_ <= 0.0) {
        throw std::invalid_argument{"fixedStepSeconds must be positive"};
    }
}

FrameSnapshot FrameCoordinator::Advance(AppController& controller, double elapsedSeconds, const GestureInput& gesture) {
    accumulator_ += std::clamp(elapsedSeconds, 0.0, 0.25);
    auto& state = controller.MutableState();
    while (accumulator_ >= fixedStepSeconds_) {
        Update(state, gesture);
        accumulator_ -= fixedStepSeconds_;
    }
    ++frameIndex_;
    return {frameIndex_, static_cast<float>(accumulator_ / fixedStepSeconds_), &state};
}

void FrameCoordinator::Update(AppState& state, const GestureInput& gesture) {
    if (state.scene.paused) {
        return;
    }
    state.scene.simulationTimeSeconds += fixedStepSeconds_;
    if (!gesture.tracked) {
        return;
    }

    const float xSign = state.gesture.invertX ? -1.0f : 1.0f;
    const float ySign = state.gesture.invertY ? -1.0f : 1.0f;
    state.scene.rotationX += gesture.rotationDeltaX * state.gesture.sensitivity * xSign;
    state.scene.rotationY += gesture.rotationDeltaY * state.gesture.sensitivity * ySign;
    state.scene.zoom = Clamp(state.scene.zoom + gesture.zoomDelta * state.gesture.sensitivity, 0.1f, 10.0f);
}

} // namespace ParticleSaturn::App
