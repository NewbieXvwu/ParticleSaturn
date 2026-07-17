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
        } else if constexpr (std::is_same_v<Command, SetVSyncMode>) {
            const int mode = std::clamp(value.value, -1, 1);
            effect.renderSettingsChanged = state_.render.vsyncMode != mode;
            state_.render.vsyncMode = mode;
        } else if constexpr (std::is_same_v<Command, SetBloomEnabled>) {
            effect.renderSettingsChanged = state_.render.bloomEnabled != value.value;
            state_.render.bloomEnabled = value.value;
        } else if constexpr (std::is_same_v<Command, SetBlurEnabled>) {
            effect.renderSettingsChanged = state_.ui.blurEnabled != value.value;
            state_.ui.blurEnabled = value.value;
        } else if constexpr (std::is_same_v<Command, SetBlurStrength>) {
            const float strength = Clamp(value.value, 0.0f, 5.0f);
            effect.renderSettingsChanged = state_.ui.blurStrength != strength;
            state_.ui.blurStrength = strength;
        } else if constexpr (std::is_same_v<Command, SetFullscreen>) {
            effect.windowChanged = state_.window.fullscreen != value.value;
            state_.window.fullscreen = value.value;
        } else if constexpr (std::is_same_v<Command, SetWindowMaterial>) {
            effect.windowChanged = state_.window.material != value.value;
            state_.window.material = value.value;
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
