#include "AppController.h"

#include <type_traits>

namespace ParticleSaturn::App {

AppController::AppController(AppState initialState) : state_{initialState} {}

const AppState& AppController::State() const noexcept {
    return state_;
}

AppState& AppController::MutableState() noexcept {
    return state_;
}

CommandEffect AppController::Dispatch(const AppCommand& command) {
    return std::visit([this](const auto& value) -> CommandEffect {
        using Command = std::decay_t<decltype(value)>;
        CommandEffect effect;

        if constexpr (std::is_same_v<Command, SetParticleCount>) {
            const auto count = std::clamp(value.value, RenderSettings::MinParticles, RenderSettings::MaxParticles);
            effect.renderSettingsChanged = state_.render.particleCount != count;
            state_.render.particleCount = count;
        } else if constexpr (std::is_same_v<Command, SetPixelRatio>) {
            const float ratio = Clamp(value.value, 0.25f, 1.0f);
            effect.renderSettingsChanged = state_.render.pixelRatio != ratio;
            state_.render.pixelRatio = ratio;
        } else if constexpr (std::is_same_v<Command, SetDensityCompensation>) {
            const float density = Clamp(value.value, 0.0f, 2.0f);
            effect.renderSettingsChanged = state_.render.densityCompensation != density;
            state_.render.densityCompensation = density;
        } else if constexpr (std::is_same_v<Command, SetVSyncMode>) {
            const int mode = std::clamp(value.value, -1, 1);
            effect.renderSettingsChanged = state_.render.vsyncMode != mode;
            state_.render.vsyncMode = mode;
        } else if constexpr (std::is_same_v<Command, SetBloomEnabled>) {
            effect.renderSettingsChanged = state_.render.bloomEnabled != value.value;
            state_.render.bloomEnabled = value.value;
        } else if constexpr (std::is_same_v<Command, SetAnalyticParticles>) {
            effect.renderSettingsChanged = state_.render.analyticParticles != value.value;
            state_.render.analyticParticles = value.value;
        } else if constexpr (std::is_same_v<Command, SetBlurEnabled>) {
            effect.renderSettingsChanged = state_.ui.blurEnabled != value.value;
            state_.ui.blurEnabled = value.value;
        } else if constexpr (std::is_same_v<Command, SetBlurStrength>) {
            const float strength = Clamp(value.value, 0.0f, 5.0f);
            effect.renderSettingsChanged = state_.ui.blurStrength != strength;
            state_.ui.blurStrength = strength;
        } else if constexpr (std::is_same_v<Command, SetDarkMode>) {
            effect.renderSettingsChanged = state_.ui.darkMode != value.value;
            state_.ui.darkMode = value.value;
        } else if constexpr (std::is_same_v<Command, SetNoiseIntensity>) {
            const float intensity = Clamp(value.value, 0.0f, 1.0f);
            effect.renderSettingsChanged = state_.ui.noiseIntensity != intensity;
            state_.ui.noiseIntensity = intensity;
        } else if constexpr (std::is_same_v<Command, SetGestureSensitivity>) {
            state_.gesture.sensitivity = Clamp(value.value, 0.1f, 5.0f);
        } else if constexpr (std::is_same_v<Command, SetGestureInvertX>) {
            state_.gesture.invertX = value.value;
        } else if constexpr (std::is_same_v<Command, SetGestureInvertY>) {
            state_.gesture.invertY = value.value;
        } else if constexpr (std::is_same_v<Command, SetHandLostDelay>) {
            state_.gesture.handLostDelay = std::clamp(value.value, 0, 120);
        } else if constexpr (std::is_same_v<Command, SetFullscreen>) {
            effect.windowChanged = state_.window.fullscreen != value.value;
            if (effect.windowChanged && value.value) {
                state_.window.windowedX = state_.window.x;
                state_.window.windowedY = state_.window.y;
                state_.window.windowedWidth = state_.window.width;
                state_.window.windowedHeight = state_.window.height;
            }
            state_.window.fullscreen = value.value;
        } else if constexpr (std::is_same_v<Command, SetWindowMaterial>) {
            effect.windowChanged = state_.window.material != value.value;
            state_.window.material = value.value;
        } else if constexpr (std::is_same_v<Command, SetWindowBounds>) {
            const auto width = std::clamp(value.width, 320U, 7680U);
            const auto height = std::clamp(value.height, 240U, 4320U);
            effect.windowChanged = state_.window.x != value.x || state_.window.y != value.y ||
                state_.window.width != width || state_.window.height != height;
            state_.window.x = value.x;
            state_.window.y = value.y;
            state_.window.width = width;
            state_.window.height = height;
            if (!state_.window.fullscreen) {
                state_.window.windowedX = value.x;
                state_.window.windowedY = value.y;
                state_.window.windowedWidth = width;
                state_.window.windowedHeight = height;
            }
        } else if constexpr (std::is_same_v<Command, SetGraphicsApi>) {
            effect.restartRequired = state_.render.graphicsApi != value.value;
            state_.render.graphicsApi = value.value;
        } else if constexpr (std::is_same_v<Command, SetVulkanDriver>) {
            effect.restartRequired = state_.render.vulkanDriver != value.value;
            state_.render.vulkanDriver = value.value;
        } else if constexpr (std::is_same_v<Command, SetLodLocked>) {
            effect.renderSettingsChanged = state_.lod.locked != value.value;
            state_.lod.locked = value.value;
        } else if constexpr (std::is_same_v<Command, ToggleDebugWindow>) {
            state_.ui.showDebugWindow = !state_.ui.showDebugWindow;
        } else if constexpr (std::is_same_v<Command, TogglePause>) {
            state_.scene.paused = !state_.scene.paused;
        }
        return effect;
    }, command);
}

} // namespace ParticleSaturn::App
