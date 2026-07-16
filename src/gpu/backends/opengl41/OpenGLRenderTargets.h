#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets {
public:
    bool Create(std::uint32_t width, std::uint32_t height);
    std::uint32_t SceneFramebuffer() const noexcept;
    std::uint32_t BloomStrongFramebuffer() const noexcept;
    std::uint32_t BloomWeakFramebuffer() const noexcept;

private:
    std::uint32_t sceneFramebuffer_ = 0;
    std::uint32_t bloomStrongFramebuffer_ = 0;
    std::uint32_t bloomWeakFramebuffer_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
