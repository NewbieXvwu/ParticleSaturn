#pragma once

#include "app/state/AppStates.h"

namespace ParticleSaturn::Platform::MacOS {

struct BackendSelection {
    App::GraphicsApi graphicsApi;
    bool             acceptedOverride = true;
};

BackendSelection SelectBackend(App::GraphicsApi persistedApi, const char* environmentOverride);

} // namespace ParticleSaturn::Platform::MacOS
