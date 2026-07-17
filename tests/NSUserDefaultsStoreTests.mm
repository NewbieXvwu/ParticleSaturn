#import <Foundation/Foundation.h>

#include "services/settings/macos/NSUserDefaultsStore.h"

#include <cassert>
#include <cmath>

int main() {
    @autoreleasepool {
        const auto* suiteName = [[NSString stringWithFormat:@"ParticleSaturnSettingsTests.%@", [NSUUID UUID].UUIDString] UTF8String];
        auto* values = [[NSUserDefaults alloc] initWithSuiteName:[NSString stringWithUTF8String:suiteName]];
        ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore store{values};
        ParticleSaturn::App::AppState state;
        state.scene.rotationX = -0.45f; state.scene.rotationY = 1.25f; state.scene.zoom = 1.75f;
        state.scene.randomSeed = 0x10203040U; state.scene.paused = true;
        state.render.particleCount = 777777U; state.render.pixelRatio = 0.8f; state.render.densityCompensation = 0.91f;
        state.render.vsyncMode = 1; state.render.bloomEnabled = false; state.render.bloomBlurStrength = 3.5f;
        state.render.graphicsApi = ParticleSaturn::App::GraphicsApi::Metal;
        state.render.vulkanDriver = ParticleSaturn::App::VulkanDriver::KosmicKrisp;
        state.ui.showDebugWindow = true; state.ui.showCameraDebug = true; state.ui.darkMode = false;
        state.ui.blurEnabled = false; state.ui.blurStrength = 4.0f; state.ui.noiseIntensity = 0.2f;
        state.gesture.sensitivity = 2.2f; state.gesture.invertX = true; state.gesture.invertY = true; state.gesture.handLostDelay = 42;
        state.lod.locked = true;
        state.window.width = 1440U; state.window.height = 900U; state.window.dpiScale = 2.0f;
        state.window.fullscreen = true; state.window.material = ParticleSaturn::App::WindowMaterial::SystemBlur;
        store.Save(state);
        const auto loaded = store.Load({});
        assert(std::abs(loaded.scene.rotationX - state.scene.rotationX) < 0.0001f);
        assert(loaded.scene.randomSeed == state.scene.randomSeed && loaded.scene.paused == state.scene.paused);
        assert(loaded.render.particleCount == state.render.particleCount && loaded.render.graphicsApi == state.render.graphicsApi);
        assert(std::abs(loaded.render.densityCompensation - state.render.densityCompensation) < 0.0001f);
        assert(loaded.ui.showDebugWindow == state.ui.showDebugWindow && loaded.ui.darkMode == state.ui.darkMode);
        assert(loaded.gesture.handLostDelay == state.gesture.handLostDelay && loaded.gesture.invertX == state.gesture.invertX);
        assert(loaded.lod.locked == state.lod.locked);
        assert(loaded.window.width == state.window.width && loaded.window.material == state.window.material);
        [values removePersistentDomainForName:[NSString stringWithUTF8String:suiteName]];
        [values release];
    }
    return 0;
}
