#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets {
public:
    bool Create(std::uint32_t width, std::uint32_t height);
    std::uint32_t SceneFramebuffer() const noexcept;
    std::uint32_t BloomStrongFramebuffer() const noexcept;
    std::uint32_t BloomWeakFramebuffer() const noexcept;
    std::uint32_t SceneTexture() const noexcept;
    std::uint32_t BloomStrongTexture() const noexcept;
    std::uint32_t BloomWeakTexture() const noexcept;
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;

private:
    std::uint32_t sceneFramebuffer_ = 0;
    std::uint32_t bloomStrongFramebuffer_ = 0;
    std::uint32_t bloomWeakFramebuffer_ = 0;
    std::uint32_t sceneTexture_ = 0;
    std::uint32_t bloomStrongTexture_ = 0;
    std::uint32_t bloomWeakTexture_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
