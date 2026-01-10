#pragma once

#include <cstdint>

namespace ParticleSaturn::Render {

enum class Backend : uint8_t {
    D3D12,
    Vulkan,
};

struct SurfaceSize {
    uint32_t Width  = 0;
    uint32_t Height = 0;
};

} // namespace ParticleSaturn::Render
