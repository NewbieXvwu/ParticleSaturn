#include "platform/macos/MacOSBackendSelection.h"

#include <cassert>

using ParticleSaturn::App::GraphicsApi;
using ParticleSaturn::Platform::MacOS::SelectBackend;

int main() {
    assert(SelectBackend(GraphicsApi::Metal, nullptr).graphicsApi == GraphicsApi::Metal);
    assert(SelectBackend(GraphicsApi::Vulkan, "").graphicsApi == GraphicsApi::Vulkan);
    assert(SelectBackend(GraphicsApi::Vulkan, "metal").graphicsApi == GraphicsApi::Metal);
    assert(SelectBackend(GraphicsApi::Metal, "opengl41").graphicsApi == GraphicsApi::OpenGL41);
    assert(SelectBackend(GraphicsApi::OpenGL41, "vulkan").graphicsApi == GraphicsApi::Vulkan);
    const auto invalid = SelectBackend(GraphicsApi::OpenGL41, "invalid");
    assert(!invalid.acceptedOverride && invalid.graphicsApi == GraphicsApi::OpenGL41);
    return 0;
}
