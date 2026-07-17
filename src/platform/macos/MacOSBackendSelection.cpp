#include "MacOSBackendSelection.h"

#include <cstring>

namespace ParticleSaturn::Platform::MacOS {

BackendSelection SelectBackend(App::GraphicsApi persistedApi, const char* environmentOverride) {
    if (environmentOverride == nullptr || environmentOverride[0] == '\0') return {persistedApi, true};
    if (std::strcmp(environmentOverride, "metal") == 0) return {App::GraphicsApi::Metal, true};
    if (std::strcmp(environmentOverride, "opengl41") == 0) return {App::GraphicsApi::OpenGL41, true};
    if (std::strcmp(environmentOverride, "vulkan") == 0) return {App::GraphicsApi::Vulkan, true};
    return {persistedApi, false};
}

} // namespace ParticleSaturn::Platform::MacOS
