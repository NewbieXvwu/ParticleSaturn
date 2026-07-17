#include "services/hand_tracking/macos/XnnpackRuntime.h"

#include <cassert>
#include <string>

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
    return 0;
}
