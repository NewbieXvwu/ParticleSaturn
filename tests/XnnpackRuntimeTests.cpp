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
    using namespace ParticleSaturn::Services;
    {
        Camera::Frame frame{3, 2, 0, 12, Camera::PixelFormat::BGRA32, Camera::FrameOrientation::Up, false,
                            {
                                10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,
                                100, 110, 120, 255, 130, 140, 150, 255, 160, 170, 180, 255,
                            }};
        std::vector<float> tensor(2U * 2U * 3U);
        std::string preprocessingError;
        assert(HandTracking::MacOS::PreprocessCameraFrameToTensor(frame, 2, 2, tensor.data(), preprocessingError));
        AssertNear(tensor[0], 30.0f / 255.0f);
        AssertNear(tensor[1], 20.0f / 255.0f);
        AssertNear(tensor[2], 10.0f / 255.0f);
        AssertNear(tensor[3], 60.0f / 255.0f);
        AssertNear(tensor[9], 150.0f / 255.0f);
        frame.orientation = Camera::FrameOrientation::Down;
        frame.mirrored = true;
        assert(HandTracking::MacOS::PreprocessCameraFrameToTensor(frame, 1, 1, tensor.data(), preprocessingError));
        AssertNear(tensor[0], 120.0f / 255.0f);
        assert(!HandTracking::MacOS::PreprocessCameraFrameToTensor(
            Camera::Frame{}, 2, 2, tensor.data(), preprocessingError));

        Camera::Frame fastPath{8, 1, 0, 24, Camera::PixelFormat::RGB24, Camera::FrameOrientation::Up, false,
                               std::vector<std::uint8_t>(24U)};
        for (std::uint32_t x = 0; x < fastPath.width; ++x) {
            fastPath.pixels[x * 3U] = static_cast<std::uint8_t>(10U + x);
            fastPath.pixels[x * 3U + 1U] = static_cast<std::uint8_t>(30U + x);
            fastPath.pixels[x * 3U + 2U] = static_cast<std::uint8_t>(50U + x);
        }
        std::vector<float> fastTensor(24U);
        assert(HandTracking::MacOS::PreprocessCameraFrameToTensor(fastPath, 8, 1, fastTensor.data(), preprocessingError));
        AssertNear(fastTensor[21], 17.0f / 255.0f);
        AssertNear(fastTensor[22], 37.0f / 255.0f);
        AssertNear(fastTensor[23], 57.0f / 255.0f);
    }
    ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime runtime;
    std::string error;
    assert(runtime.Load(PARTICLESATURN_PALM_MODEL_PATH, PARTICLESATURN_LANDMARK_MODEL_PATH, error));
    assert(runtime.IsLoaded());
    ParticleSaturn::Services::Camera::Frame frame{64, 48, 0, 64U * 3U,
        ParticleSaturn::Services::Camera::PixelFormat::RGB24,
        ParticleSaturn::Services::Camera::FrameOrientation::Up, false,
        std::vector<std::uint8_t>(64U * 48U * 3U, 127U)};
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
