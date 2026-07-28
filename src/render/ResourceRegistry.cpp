#include "ResourceRegistry.h"

#include <stdexcept>

namespace ParticleSaturn::Render {

Gpu::TextureHandle TexturePool::Acquire(const Gpu::TextureDesc& desc, std::uint64_t completedFrame) {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) {
        throw std::invalid_argument{"texture dimensions and mip count must be non-zero"};
    }
    for (std::uint32_t index = 0; index < entries_.size(); ++index) {
        auto& entry = entries_[index];
        if (!entry.leased && entry.availableAfterFrame <= completedFrame && entry.desc.width == desc.width &&
            entry.desc.height == desc.height && entry.desc.mipLevels == desc.mipLevels) {
            entry.leased = true;
            return {index, entry.generation};
        }
    }
    entries_.push_back({desc, 1, 0, true});
    return {static_cast<std::uint32_t>(entries_.size() - 1), 1};
}

void TexturePool::Release(Gpu::TextureHandle texture, std::uint64_t retireAfterFrame) {
    if (!texture || texture.index >= entries_.size()) {
        throw std::out_of_range{"texture handle does not belong to this pool"};
    }
    auto& entry = entries_[texture.index];
    if (!entry.leased || entry.generation != texture.generation) {
        throw std::logic_error{"texture handle is stale or already released"};
    }
    entry.leased              = false;
    entry.availableAfterFrame = retireAfterFrame;
    ++entry.generation;
    if (entry.generation == 0) {
        entry.generation = 1;
    }
}

std::size_t TexturePool::LiveCount() const noexcept {
    return entries_.size();
}

} // namespace ParticleSaturn::Render
