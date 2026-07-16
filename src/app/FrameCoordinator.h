#pragma once

#include "AppController.h"

#include <cstdint>

namespace ParticleSaturn::App {

struct GestureInput {
    bool tracked = false;
    float rotationDeltaX = 0.0f;
    float rotationDeltaY = 0.0f;
    float zoomDelta = 0.0f;
};

struct FrameSnapshot {
    std::uint64_t frameIndex = 0;
    float interpolation = 0.0f;
    const AppState* state = nullptr;
};

class FrameCoordinator {
public:
    explicit FrameCoordinator(double fixedStepSeconds = 1.0 / 120.0);

    FrameSnapshot Advance(AppController& controller, double elapsedSeconds, const GestureInput& gesture = {});

private:
    void Update(AppState& state, const GestureInput& gesture);

    double fixedStepSeconds_;
    double accumulator_ = 0.0;
    std::uint64_t frameIndex_ = 0;
};

} // namespace ParticleSaturn::App
