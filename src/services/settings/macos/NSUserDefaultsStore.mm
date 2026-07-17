#import <Foundation/Foundation.h>

#include "NSUserDefaultsStore.h"

namespace ParticleSaturn::Services::Settings::MacOS {

namespace {

constexpr const char* ParticleCountKey = "render.particleCount";
constexpr const char* PixelRatioKey = "render.pixelRatio";
constexpr const char* DensityCompensationKey = "render.densityCompensation";
constexpr const char* VSyncKey = "render.vsyncMode";
constexpr const char* BloomKey = "render.bloomEnabled";
constexpr const char* BloomBlurStrengthKey = "render.bloomBlurStrength";
constexpr const char* GraphicsApiKey = "render.graphicsApi";
constexpr const char* VulkanDriverKey = "render.vulkanDriver";
constexpr const char* ShowDebugWindowKey = "ui.showDebugWindow";
constexpr const char* ShowCameraDebugKey = "ui.showCameraDebug";
constexpr const char* DarkModeKey = "ui.darkMode";
constexpr const char* BlurKey = "ui.blurEnabled";
constexpr const char* BlurStrengthKey = "ui.blurStrength";
constexpr const char* NoiseIntensityKey = "ui.noiseIntensity";
constexpr const char* GestureSensitivityKey = "gesture.sensitivity";
constexpr const char* GestureInvertXKey = "gesture.invertX";
constexpr const char* GestureInvertYKey = "gesture.invertY";
constexpr const char* HandLostDelayKey = "gesture.handLostDelay";
constexpr const char* LodLockedKey = "lod.locked";
constexpr const char* RotationXKey = "scene.rotationX";
constexpr const char* RotationYKey = "scene.rotationY";
constexpr const char* ZoomKey = "scene.zoom";
constexpr const char* RandomSeedKey = "scene.randomSeed";
constexpr const char* PausedKey = "scene.paused";
constexpr const char* WindowWidthKey = "window.width";
constexpr const char* WindowHeightKey = "window.height";
constexpr const char* WindowXKey = "window.x";
constexpr const char* WindowYKey = "window.y";
constexpr const char* WindowedXKey = "window.windowedX";
constexpr const char* WindowedYKey = "window.windowedY";
constexpr const char* WindowedWidthKey = "window.windowedWidth";
constexpr const char* WindowedHeightKey = "window.windowedHeight";
constexpr const char* WindowDpiScaleKey = "window.dpiScale";
constexpr const char* FullscreenKey = "window.fullscreen";
constexpr const char* MaterialKey = "window.material";

bool HasValue(NSUserDefaults* defaults, const char* key) {
    return [defaults objectForKey:[NSString stringWithUTF8String:key]] != nil;
}

NSUserDefaults* Values(void* nativeDefaults) {
    return nativeDefaults == nullptr ? [NSUserDefaults standardUserDefaults] : (NSUserDefaults*)nativeDefaults;
}

template <class Enum>
Enum ValidEnum(NSInteger value, Enum fallback, NSInteger maximum) {
    return value >= 0 && value <= maximum ? static_cast<Enum>(value) : fallback;
}

} // namespace

App::AppState NSUserDefaultsStore::Load(const App::AppState& defaults) const {
    auto state = defaults;
    auto* values = Values(nativeDefaults_);
    if (HasValue(values, ParticleCountKey)) state.render.particleCount = static_cast<std::uint32_t>([values integerForKey:@"render.particleCount"]);
    if (HasValue(values, PixelRatioKey)) state.render.pixelRatio = [values floatForKey:@"render.pixelRatio"];
    if (HasValue(values, DensityCompensationKey)) state.render.densityCompensation = [values floatForKey:@"render.densityCompensation"];
    if (HasValue(values, VSyncKey)) state.render.vsyncMode = static_cast<int>([values integerForKey:@"render.vsyncMode"]);
    if (HasValue(values, BloomKey)) state.render.bloomEnabled = [values boolForKey:@"render.bloomEnabled"];
    if (HasValue(values, BloomBlurStrengthKey)) state.render.bloomBlurStrength = [values floatForKey:@"render.bloomBlurStrength"];
    if (HasValue(values, GraphicsApiKey)) state.render.graphicsApi = ValidEnum([values integerForKey:@"render.graphicsApi"], state.render.graphicsApi, 2);
    if (HasValue(values, VulkanDriverKey)) state.render.vulkanDriver = ValidEnum([values integerForKey:@"render.vulkanDriver"], state.render.vulkanDriver, 1);
    if (HasValue(values, ShowDebugWindowKey)) state.ui.showDebugWindow = [values boolForKey:@"ui.showDebugWindow"];
    if (HasValue(values, ShowCameraDebugKey)) state.ui.showCameraDebug = [values boolForKey:@"ui.showCameraDebug"];
    if (HasValue(values, DarkModeKey)) state.ui.darkMode = [values boolForKey:@"ui.darkMode"];
    if (HasValue(values, BlurKey)) state.ui.blurEnabled = [values boolForKey:@"ui.blurEnabled"];
    if (HasValue(values, BlurStrengthKey)) state.ui.blurStrength = [values floatForKey:@"ui.blurStrength"];
    if (HasValue(values, NoiseIntensityKey)) state.ui.noiseIntensity = [values floatForKey:@"ui.noiseIntensity"];
    if (HasValue(values, GestureSensitivityKey)) state.gesture.sensitivity = [values floatForKey:@"gesture.sensitivity"];
    if (HasValue(values, GestureInvertXKey)) state.gesture.invertX = [values boolForKey:@"gesture.invertX"];
    if (HasValue(values, GestureInvertYKey)) state.gesture.invertY = [values boolForKey:@"gesture.invertY"];
    if (HasValue(values, HandLostDelayKey)) state.gesture.handLostDelay = static_cast<int>([values integerForKey:@"gesture.handLostDelay"]);
    if (HasValue(values, LodLockedKey)) state.lod.locked = [values boolForKey:@"lod.locked"];
    if (HasValue(values, RotationXKey)) state.scene.rotationX = [values floatForKey:@"scene.rotationX"];
    if (HasValue(values, RotationYKey)) state.scene.rotationY = [values floatForKey:@"scene.rotationY"];
    if (HasValue(values, ZoomKey)) state.scene.zoom = [values floatForKey:@"scene.zoom"];
    if (HasValue(values, RandomSeedKey)) state.scene.randomSeed = static_cast<std::uint32_t>([values integerForKey:@"scene.randomSeed"]);
    if (HasValue(values, PausedKey)) state.scene.paused = [values boolForKey:@"scene.paused"];
    if (HasValue(values, WindowWidthKey)) state.window.width = static_cast<std::uint32_t>([values integerForKey:@"window.width"]);
    if (HasValue(values, WindowHeightKey)) state.window.height = static_cast<std::uint32_t>([values integerForKey:@"window.height"]);
    if (HasValue(values, WindowXKey)) state.window.x = static_cast<std::int32_t>([values integerForKey:@"window.x"]);
    if (HasValue(values, WindowYKey)) state.window.y = static_cast<std::int32_t>([values integerForKey:@"window.y"]);
    if (HasValue(values, WindowedXKey)) state.window.windowedX = static_cast<std::int32_t>([values integerForKey:@"window.windowedX"]);
    if (HasValue(values, WindowedYKey)) state.window.windowedY = static_cast<std::int32_t>([values integerForKey:@"window.windowedY"]);
    if (HasValue(values, WindowedWidthKey)) state.window.windowedWidth = static_cast<std::uint32_t>([values integerForKey:@"window.windowedWidth"]);
    if (HasValue(values, WindowedHeightKey)) state.window.windowedHeight = static_cast<std::uint32_t>([values integerForKey:@"window.windowedHeight"]);
    if (HasValue(values, WindowDpiScaleKey)) state.window.dpiScale = [values floatForKey:@"window.dpiScale"];
    if (HasValue(values, FullscreenKey)) state.window.fullscreen = [values boolForKey:@"window.fullscreen"];
    if (HasValue(values, MaterialKey)) state.window.material = ValidEnum([values integerForKey:@"window.material"], state.window.material, 3);
    state.render.particleCount = std::clamp(state.render.particleCount, App::RenderSettings::MinParticles, App::RenderSettings::MaxParticles);
    state.render.pixelRatio = App::Clamp(state.render.pixelRatio, 0.25f, 1.0f);
    state.render.densityCompensation = App::Clamp(state.render.densityCompensation, 0.0f, 2.0f);
    state.render.bloomBlurStrength = App::Clamp(state.render.bloomBlurStrength, 0.0f, 5.0f);
    state.ui.blurStrength = App::Clamp(state.ui.blurStrength, 0.0f, 5.0f);
    state.ui.noiseIntensity = App::Clamp(state.ui.noiseIntensity, 0.0f, 1.0f);
    state.gesture.sensitivity = App::Clamp(state.gesture.sensitivity, 0.1f, 5.0f);
    state.gesture.handLostDelay = std::clamp(state.gesture.handLostDelay, 0, 120);
    state.scene.zoom = App::Clamp(state.scene.zoom, 0.1f, 10.0f);
    state.window.width = std::clamp(state.window.width, 320U, 7680U);
    state.window.height = std::clamp(state.window.height, 240U, 4320U);
    state.window.windowedWidth = std::clamp(state.window.windowedWidth, 320U, 7680U);
    state.window.windowedHeight = std::clamp(state.window.windowedHeight, 240U, 4320U);
    state.window.dpiScale = App::Clamp(state.window.dpiScale, 1.0f, 4.0f);
    return state;
}

void NSUserDefaultsStore::Save(const App::AppState& state) {
    auto* values = Values(nativeDefaults_);
    [values setInteger:state.render.particleCount forKey:@"render.particleCount"];
    [values setFloat:state.render.pixelRatio forKey:@"render.pixelRatio"];
    [values setFloat:state.render.densityCompensation forKey:@"render.densityCompensation"];
    [values setInteger:state.render.vsyncMode forKey:@"render.vsyncMode"];
    [values setBool:state.render.bloomEnabled forKey:@"render.bloomEnabled"];
    [values setFloat:state.render.bloomBlurStrength forKey:@"render.bloomBlurStrength"];
    [values setInteger:static_cast<NSInteger>(state.render.graphicsApi) forKey:@"render.graphicsApi"];
    [values setInteger:static_cast<NSInteger>(state.render.vulkanDriver) forKey:@"render.vulkanDriver"];
    [values setBool:state.ui.showDebugWindow forKey:@"ui.showDebugWindow"];
    [values setBool:state.ui.showCameraDebug forKey:@"ui.showCameraDebug"];
    [values setBool:state.ui.darkMode forKey:@"ui.darkMode"];
    [values setBool:state.ui.blurEnabled forKey:@"ui.blurEnabled"];
    [values setFloat:state.ui.blurStrength forKey:@"ui.blurStrength"];
    [values setFloat:state.ui.noiseIntensity forKey:@"ui.noiseIntensity"];
    [values setFloat:state.gesture.sensitivity forKey:@"gesture.sensitivity"];
    [values setBool:state.gesture.invertX forKey:@"gesture.invertX"];
    [values setBool:state.gesture.invertY forKey:@"gesture.invertY"];
    [values setInteger:state.gesture.handLostDelay forKey:@"gesture.handLostDelay"];
    [values setBool:state.lod.locked forKey:@"lod.locked"];
    [values setFloat:state.scene.rotationX forKey:@"scene.rotationX"];
    [values setFloat:state.scene.rotationY forKey:@"scene.rotationY"];
    [values setFloat:state.scene.zoom forKey:@"scene.zoom"];
    [values setInteger:state.scene.randomSeed forKey:@"scene.randomSeed"];
    [values setBool:state.scene.paused forKey:@"scene.paused"];
    [values setInteger:state.window.width forKey:@"window.width"];
    [values setInteger:state.window.height forKey:@"window.height"];
    [values setInteger:state.window.x forKey:@"window.x"];
    [values setInteger:state.window.y forKey:@"window.y"];
    [values setInteger:state.window.windowedX forKey:@"window.windowedX"];
    [values setInteger:state.window.windowedY forKey:@"window.windowedY"];
    [values setInteger:state.window.windowedWidth forKey:@"window.windowedWidth"];
    [values setInteger:state.window.windowedHeight forKey:@"window.windowedHeight"];
    [values setFloat:state.window.dpiScale forKey:@"window.dpiScale"];
    [values setBool:state.window.fullscreen forKey:@"window.fullscreen"];
    [values setInteger:static_cast<NSInteger>(state.window.material) forKey:@"window.material"];
    [values synchronize];
}

} // namespace ParticleSaturn::Services::Settings::MacOS
