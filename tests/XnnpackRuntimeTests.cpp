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
    return 0;
}
