#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets {
public:
    ~OpenGLRenderTargets();
    OpenGLRenderTargets() = default;
    OpenGLRenderTargets(const OpenGLRenderTargets&) = delete;
    OpenGLRenderTargets& operator=(const OpenGLRenderTargets&) = delete;

    bool Create(std::uint32_t width, std::uint32_t height);
    std::uint32_t SceneFramebuffer() const noexcept;
    std::uint32_t BloomStrongFramebuffer() const noexcept;
    std::uint32_t BloomPingPongFramebuffer() const noexcept;
    std::uint32_t BloomWeakFramebuffer() const noexcept;
    std::uint32_t BloomWeakPingPongFramebuffer() const noexcept;
    std::uint32_t ToneMappedFramebuffer() const noexcept;
    std::uint32_t SceneTexture() const noexcept;
    std::uint32_t BloomStrongTexture() const noexcept;
    std::uint32_t BloomPingPongTexture() const noexcept;
    std::uint32_t BloomWeakTexture() const noexcept;
    std::uint32_t BloomWeakPingPongTexture() const noexcept;
    std::uint32_t ToneMappedTexture() const noexcept;
    std::uint32_t Width() const noexcept;
    std::uint32_t Height() const noexcept;

private:
    void Destroy() noexcept;

    std::uint32_t sceneFramebuffer_ = 0;
    std::uint32_t bloomStrongFramebuffer_ = 0;
    std::uint32_t bloomPingPongFramebuffer_ = 0;
    std::uint32_t bloomWeakFramebuffer_ = 0;
    std::uint32_t bloomWeakPingPongFramebuffer_ = 0;
    std::uint32_t toneMappedFramebuffer_ = 0;
    std::uint32_t sceneTexture_ = 0;
    std::uint32_t bloomStrongTexture_ = 0;
    std::uint32_t bloomPingPongTexture_ = 0;
    std::uint32_t bloomWeakTexture_ = 0;
    std::uint32_t bloomWeakPingPongTexture_ = 0;
    std::uint32_t toneMappedTexture_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
