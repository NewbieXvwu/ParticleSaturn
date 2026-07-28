#pragma once

#include <cstddef>
#include <cstdint>

namespace ParticleSaturn::Gpu {

template <typename Tag> struct Handle {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept { return generation != 0; }

    friend bool operator==(Handle, Handle) = default;
};

struct BufferTag {};

struct TextureTag {};

using BufferHandle  = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;

enum class ResourceUsage : std::uint8_t {
    Undefined,
    CopySource,
    CopyDestination,
    ShaderRead,
    ShaderWrite,
    IndirectArgument,
    RenderTarget,
    Present,
};

enum class BufferUsage : std::uint32_t {
    None            = 0,
    Vertex          = 1U << 0U,
    Index           = 1U << 1U,
    Uniform         = 1U << 2U,
    Storage         = 1U << 3U,
    Indirect        = 1U << 4U,
    CopySource      = 1U << 5U,
    CopyDestination = 1U << 6U,
};

constexpr BufferUsage operator|(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr bool HasUsage(BufferUsage usage, BufferUsage value) noexcept {
    return (static_cast<std::uint32_t>(usage) & static_cast<std::uint32_t>(value)) != 0U;
}

struct BufferDesc {
    std::size_t size          = 0;
    std::size_t elementStride = 0;
    BufferUsage usage         = BufferUsage::None;
};

struct TextureDesc {
    std::uint32_t width     = 0;
    std::uint32_t height    = 0;
    std::uint32_t mipLevels = 1;
};

struct FrameToken {
    std::uint64_t value = 0;
};

} // namespace ParticleSaturn::Gpu
