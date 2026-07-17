#pragma once

#include "state/AppStates.h"

#include <cstdint>
#include <variant>

namespace ParticleSaturn::App {

struct SetParticleCount { std::uint32_t value; };
struct SetPixelRatio { float value; };
struct SetVSyncMode { int value; };
struct SetBloomEnabled { bool value; };
struct SetBlurEnabled { bool value; };
struct SetBlurStrength { float value; };
struct SetFullscreen { bool value; };
struct SetWindowMaterial { WindowMaterial value; };
struct SetGraphicsApi { GraphicsApi value; };
struct SetVulkanDriver { VulkanDriver value; };
struct SetLodLocked { bool value; };
struct ToggleDebugWindow {};
struct TogglePause {};

using AppCommand = std::variant<
    SetParticleCount,
    SetPixelRatio,
    SetVSyncMode,
    SetBloomEnabled,
    SetBlurEnabled,
    SetBlurStrength,
    SetFullscreen,
    SetWindowMaterial,
    SetGraphicsApi,
    SetVulkanDriver,
    SetLodLocked,
    ToggleDebugWindow,
    TogglePause>;

struct CommandEffect {
    bool renderSettingsChanged = false;
    bool windowChanged         = false;
    bool restartRequired       = false;
};

} // namespace ParticleSaturn::App
