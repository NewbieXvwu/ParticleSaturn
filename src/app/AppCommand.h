#pragma once

#include "state/AppStates.h"

#include <cstdint>
#include <variant>

namespace ParticleSaturn::App {

struct SetParticleCount { std::uint32_t value; };
struct SetPixelRatio { float value; };
struct SetDensityCompensation { float value; };
struct SetVSyncMode { int value; };
struct SetBloomEnabled { bool value; };
struct SetBlurEnabled { bool value; };
struct SetBlurStrength { float value; };
struct SetDarkMode { bool value; };
struct SetNoiseIntensity { float value; };
struct SetGestureSensitivity { float value; };
struct SetGestureInvertX { bool value; };
struct SetGestureInvertY { bool value; };
struct SetHandLostDelay { int value; };
struct SetFullscreen { bool value; };
struct SetWindowMaterial { WindowMaterial value; };
struct SetWindowBounds { std::int32_t x; std::int32_t y; std::uint32_t width; std::uint32_t height; };
struct SetGraphicsApi { GraphicsApi value; };
struct SetVulkanDriver { VulkanDriver value; };
struct SetLodLocked { bool value; };
struct ToggleDebugWindow {};
struct TogglePause {};

using AppCommand = std::variant<
    SetParticleCount,
    SetPixelRatio,
    SetDensityCompensation,
    SetVSyncMode,
    SetBloomEnabled,
    SetBlurEnabled,
    SetBlurStrength,
    SetDarkMode,
    SetNoiseIntensity,
    SetGestureSensitivity,
    SetGestureInvertX,
    SetGestureInvertY,
    SetHandLostDelay,
    SetFullscreen,
    SetWindowMaterial,
    SetWindowBounds,
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
