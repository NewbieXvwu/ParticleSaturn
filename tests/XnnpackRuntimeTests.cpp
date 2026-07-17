#include "services/hand_tracking/macos/XnnpackRuntime.h"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.001f);
}

} // namespace

int main() {
    ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime runtime;
    std::string error;
    assert(runtime.Load(PARTICLESATURN_PALM_MODEL_PATH, PARTICLESATURN_LANDMARK_MODEL_PATH, error));
    assert(runtime.IsLoaded());
    ParticleSaturn::Services::Camera::Frame frame{64, 48, 0, std::vector<std::uint8_t>(64U * 48U * 3U, 127U)};
    assert(runtime.Invoke(frame, error));
    const auto& palmOutputs = runtime.PalmOutputs();
    assert(palmOutputs.size() == 2U);
    assert((palmOutputs[0].size() == 2016U || palmOutputs[1].size() == 2016U));
    assert((palmOutputs[0].size() == 2016U * 18U || palmOutputs[1].size() == 2016U * 18U));
    const auto& landmarkOutputs = runtime.LandmarkOutputs();
    if (!landmarkOutputs.empty()) {
        assert(landmarkOutputs.size() == 4U);
        std::size_t landmarks = 0;
        std::size_t scalars = 0;
        for (const auto& output : landmarkOutputs) {
            landmarks += output.size() == 63U;
            scalars += output.size() == 1U;
        }
        assert(landmarks == 2U && scalars == 2U);
    }

    using ParticleSaturn::Services::HandTracking::MacOS::HandPose;
    using ParticleSaturn::Services::HandTracking::MacOS::PalmRegion;
    PalmRegion region;
    region.centerX = 0.5f;
    region.centerY = 0.5f;
    region.width = 0.2f;
    region.height = 0.2f;
    region.rotation = 0.0f;
    ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime::ExpandPalmToHandRegion(region);
    AssertNear(region.handCenterX, 0.5f);
    AssertNear(region.handCenterY, 0.4f);
    AssertNear(region.handSide, 0.52f);

    std::vector<float> screenLandmarks(63U, 112.0f);
    screenLandmarks[4U * 3U] = 112.0f;
    screenLandmarks[4U * 3U + 1U] = 112.0f;
    screenLandmarks[8U * 3U] = 168.0f;
    screenLandmarks[8U * 3U + 1U] = 112.0f;
    const std::vector<std::vector<float>> syntheticOutputs{
        screenLandmarks, std::vector<float>{2.0f}, std::vector<float>{0.2f}, std::vector<float>(63U)};
    HandPose pose;
    assert(ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime::DecodeLandmarkOutputs(
        syntheticOutputs, region, pose));
    AssertNear(pose.centerX, 0.5f);
    AssertNear(pose.centerY, 0.4f);
    AssertNear(pose.scale, 1.38f);

    region.isLeftHand = true;
    screenLandmarks[0] = 0.0f;
    const std::vector<std::vector<float>> leftOutputs{screenLandmarks, std::vector<float>{2.0f}};
    assert(ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime::DecodeLandmarkOutputs(
        leftOutputs, region, pose));
    AssertNear(pose.centerX, 0.76f);
    AssertNear(pose.centerY, 0.4f);
    return 0;
}
