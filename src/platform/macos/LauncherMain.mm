#import <Cocoa/Cocoa.h>

#include <cstdlib>
#include <iostream>

#include "MacOSApplication.h"
#include "MacOSBackendSelection.h"
#include "app/state/AppStates.h"
#include "services/settings/macos/NSUserDefaultsStore.h"

namespace {

ParticleSaturn::App::GraphicsApi SelectGraphicsApi() {
    ParticleSaturn::App::AppState defaults;
    defaults.render.graphicsApi = ParticleSaturn::App::GraphicsApi::Metal;
    ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
    const auto selection = ParticleSaturn::Platform::MacOS::SelectBackend(
        settings.Load(defaults).render.graphicsApi, std::getenv("PARTICLESATURN_GRAPHICS_API"));
    if (!selection.acceptedOverride) {
        std::cerr << "unknown PARTICLESATURN_GRAPHICS_API\n";
    }
    return selection.graphicsApi;
}

} // namespace

int main() {
    @autoreleasepool {
        switch (SelectGraphicsApi()) {
        case ParticleSaturn::App::GraphicsApi::Metal:
            return ParticleSaturn::Platform::MacOS::RunMetalApplication();
        case ParticleSaturn::App::GraphicsApi::OpenGL41:
            return ParticleSaturn::Platform::MacOS::RunOpenGL41Application();
        case ParticleSaturn::App::GraphicsApi::Vulkan:
            std::cerr << "Vulkan presentation is not implemented yet\n";
            return 2;
        }
    }
    return 2;
}
