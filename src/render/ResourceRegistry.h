#pragma once

#include "gpu/interface/GpuTypes.h"

#include <cstdint>
#include <vector>

namespace ParticleSaturn::Render {

class TexturePool {
public:
    Gpu::TextureHandle Acquire(const Gpu::TextureDesc& desc, std::uint64_t completedFrame);
    void Release(Gpu::TextureHandle texture, std::uint64_t retireAfterFrame);
    std::size_t LiveCount() const noexcept;

private:
    struct Entry {
        Gpu::TextureDesc desc;
        std::uint32_t generation = 1;
        std::uint64_t availableAfterFrame = 0;
        bool leased = false;
    };

    std::vector<Entry> entries_;
};

} // namespace ParticleSaturn::Render
