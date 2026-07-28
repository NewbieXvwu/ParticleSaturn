#pragma once

#include <cstdint>

#include "AppController.h"

namespace ParticleSaturn::App {

struct GestureInput {
    bool  tracked        = false;
    float rotationDeltaX = 0.0f;
    float rotationDeltaY = 0.0f;
    float zoomDelta      = 0.0f;
    // 摄像头路径使用 HandTracker 的归一化绝对姿态。保留增量字段供
    // 已有平台输入和命令测试使用。
    bool  hasAbsolutePose     = false;
    float scale               = 1.0f;
    float rotationXNormalized = 0.5f;
    float rotationYNormalized = 0.625f;
};

struct FrameSnapshot {
    std::uint64_t   frameIndex    = 0;
    float           interpolation = 0.0f;
    const AppState* state         = nullptr;
};

class FrameCoordinator {
  public:
    explicit FrameCoordinator(double fixedStepSeconds = 1.0 / 120.0);

    FrameSnapshot Advance(AppController& controller, double elapsedSeconds, const GestureInput& gesture = {});

  private:
    void Update(AppState& state, const GestureInput& gesture);
    void UpdateLod(AppState& state, double elapsedSeconds);

    double        fixedStepSeconds_;
    double        accumulator_ = 0.0;
    std::uint64_t frameIndex_  = 0;
};

} // namespace ParticleSaturn::App
