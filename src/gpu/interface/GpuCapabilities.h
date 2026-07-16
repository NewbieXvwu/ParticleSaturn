#pragma once

#include <string_view>

namespace ParticleSaturn::Gpu {

struct GpuCapabilities {
    bool supportsCompute = false;
    bool supportsStorageBuffer = false;
    bool supportsIndirectDraw = false;
    bool supportsMeshShader = false;
    bool supportsTransparentSurface = false;
    bool supportsTimestamp = false;
    bool supportsProgramCache = false;
    bool supportsAdaptiveVSync = false;
};

enum class RequiredCapability {
    Compute,
    StorageBuffer,
    IndirectDraw,
    TransparentSurface,
};

inline bool Supports(const GpuCapabilities& capabilities, RequiredCapability capability) noexcept {
    switch (capability) {
    case RequiredCapability::Compute: return capabilities.supportsCompute;
    case RequiredCapability::StorageBuffer: return capabilities.supportsStorageBuffer;
    case RequiredCapability::IndirectDraw: return capabilities.supportsIndirectDraw;
    case RequiredCapability::TransparentSurface: return capabilities.supportsTransparentSurface;
    }
    return false;
}

inline std::string_view CapabilityName(RequiredCapability capability) noexcept {
    switch (capability) {
    case RequiredCapability::Compute: return "compute";
    case RequiredCapability::StorageBuffer: return "storage-buffer";
    case RequiredCapability::IndirectDraw: return "indirect-draw";
    case RequiredCapability::TransparentSurface: return "transparent-surface";
    }
    return "unknown";
}

} // namespace ParticleSaturn::Gpu
