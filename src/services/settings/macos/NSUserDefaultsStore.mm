#import <Foundation/Foundation.h>

#include "NSUserDefaultsStore.h"

namespace ParticleSaturn::Services::Settings::MacOS {

namespace {

constexpr const char* ParticleCountKey = "render.particleCount";
constexpr const char* PixelRatioKey = "render.pixelRatio";
constexpr const char* VSyncKey = "render.vsyncMode";
constexpr const char* BloomKey = "render.bloomEnabled";
constexpr const char* GraphicsApiKey = "render.graphicsApi";
constexpr const char* VulkanDriverKey = "render.vulkanDriver";
constexpr const char* BlurKey = "ui.blurEnabled";
constexpr const char* BlurStrengthKey = "ui.blurStrength";
constexpr const char* GestureSensitivityKey = "gesture.sensitivity";
constexpr const char* GestureInvertXKey = "gesture.invertX";
constexpr const char* GestureInvertYKey = "gesture.invertY";

bool HasValue(NSUserDefaults* defaults, const char* key) {
    return [defaults objectForKey:[NSString stringWithUTF8String:key]] != nil;
}

} // namespace

App::AppState NSUserDefaultsStore::Load(const App::AppState& defaults) const {
    auto state = defaults;
    auto* values = [NSUserDefaults standardUserDefaults];
    if (HasValue(values, ParticleCountKey)) state.render.particleCount = static_cast<std::uint32_t>([values integerForKey:@"render.particleCount"]);
    if (HasValue(values, PixelRatioKey)) state.render.pixelRatio = [values floatForKey:@"render.pixelRatio"];
    if (HasValue(values, VSyncKey)) state.render.vsyncMode = static_cast<int>([values integerForKey:@"render.vsyncMode"]);
    if (HasValue(values, BloomKey)) state.render.bloomEnabled = [values boolForKey:@"render.bloomEnabled"];
    if (HasValue(values, GraphicsApiKey)) state.render.graphicsApi = static_cast<App::GraphicsApi>([values integerForKey:@"render.graphicsApi"]);
    if (HasValue(values, VulkanDriverKey)) state.render.vulkanDriver = static_cast<App::VulkanDriver>([values integerForKey:@"render.vulkanDriver"]);
    if (HasValue(values, BlurKey)) state.ui.blurEnabled = [values boolForKey:@"ui.blurEnabled"];
    if (HasValue(values, BlurStrengthKey)) state.ui.blurStrength = [values floatForKey:@"ui.blurStrength"];
    if (HasValue(values, GestureSensitivityKey)) state.gesture.sensitivity = [values floatForKey:@"gesture.sensitivity"];
    if (HasValue(values, GestureInvertXKey)) state.gesture.invertX = [values boolForKey:@"gesture.invertX"];
    if (HasValue(values, GestureInvertYKey)) state.gesture.invertY = [values boolForKey:@"gesture.invertY"];
    state.render.particleCount = std::clamp(state.render.particleCount, App::RenderSettings::MinParticles, App::RenderSettings::MaxParticles);
    state.render.pixelRatio = App::Clamp(state.render.pixelRatio, 0.25f, 1.0f);
    state.ui.blurStrength = App::Clamp(state.ui.blurStrength, 0.0f, 5.0f);
    state.gesture.sensitivity = App::Clamp(state.gesture.sensitivity, 0.1f, 5.0f);
    return state;
}

void NSUserDefaultsStore::Save(const App::AppState& state) {
    auto* values = [NSUserDefaults standardUserDefaults];
    [values setInteger:state.render.particleCount forKey:@"render.particleCount"];
    [values setFloat:state.render.pixelRatio forKey:@"render.pixelRatio"];
    [values setInteger:state.render.vsyncMode forKey:@"render.vsyncMode"];
    [values setBool:state.render.bloomEnabled forKey:@"render.bloomEnabled"];
    [values setInteger:static_cast<NSInteger>(state.render.graphicsApi) forKey:@"render.graphicsApi"];
    [values setInteger:static_cast<NSInteger>(state.render.vulkanDriver) forKey:@"render.vulkanDriver"];
    [values setBool:state.ui.blurEnabled forKey:@"ui.blurEnabled"];
    [values setFloat:state.ui.blurStrength forKey:@"ui.blurStrength"];
    [values setFloat:state.gesture.sensitivity forKey:@"gesture.sensitivity"];
    [values setBool:state.gesture.invertX forKey:@"gesture.invertX"];
    [values setBool:state.gesture.invertY forKey:@"gesture.invertY"];
    [values synchronize];
}

} // namespace ParticleSaturn::Services::Settings::MacOS
