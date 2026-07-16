#import <OpenGL/gl3.h>

#include "OpenGLRenderTargets.h"

#include <algorithm>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

bool CreateTarget(std::uint32_t width, std::uint32_t height, GLenum format, GLenum sourceFormat, GLenum sourceType,
                  std::uint32_t& framebuffer, std::uint32_t& texture) {
    glGenFramebuffers(1, &framebuffer);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, sourceFormat, sourceType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

} // namespace

OpenGLRenderTargets::~OpenGLRenderTargets() { Destroy(); }

void OpenGLRenderTargets::Destroy() noexcept {
    const std::uint32_t framebuffers[] = {
        sceneFramebuffer_, bloomStrongFramebuffer_, bloomPingPongFramebuffer_, bloomWeakFramebuffer_,
        bloomWeakPingPongFramebuffer_, toneMappedFramebuffer_,
    };
    const std::uint32_t textures[] = {
        sceneTexture_, bloomStrongTexture_, bloomPingPongTexture_, bloomWeakTexture_, bloomWeakPingPongTexture_,
        toneMappedTexture_,
    };
    glDeleteFramebuffers(static_cast<GLsizei>(std::size(framebuffers)), framebuffers);
    glDeleteTextures(static_cast<GLsizei>(std::size(textures)), textures);
    sceneFramebuffer_ = bloomStrongFramebuffer_ = bloomPingPongFramebuffer_ = 0;
    bloomWeakFramebuffer_ = bloomWeakPingPongFramebuffer_ = toneMappedFramebuffer_ = 0;
    sceneTexture_ = bloomStrongTexture_ = bloomPingPongTexture_ = 0;
    bloomWeakTexture_ = bloomWeakPingPongTexture_ = toneMappedTexture_ = 0;
    width_ = height_ = 0;
}

bool OpenGLRenderTargets::Create(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return false;
    Destroy();
    const auto strongWidth = std::max(1U, width / 6U);
    const auto strongHeight = std::max(1U, height / 6U);
    const auto weakWidth = std::max(1U, width / 12U);
    const auto weakHeight = std::max(1U, height / 12U);
    // Match the Metal reference path. R11G11B10F produces visible quantization
    // steps after the 1.2M-particle additive blend on the macOS OpenGL driver.
    const bool scene = CreateTarget(width, height, GL_RGBA16F, GL_RGBA, GL_FLOAT, sceneFramebuffer_, sceneTexture_);
    const bool strong = CreateTarget(strongWidth, strongHeight, GL_RGBA16F, GL_RGBA, GL_FLOAT,
                                    bloomStrongFramebuffer_, bloomStrongTexture_);
    const bool strongPingPong = CreateTarget(strongWidth, strongHeight, GL_RGBA16F, GL_RGBA, GL_FLOAT,
                                             bloomPingPongFramebuffer_, bloomPingPongTexture_);
    const bool weak = CreateTarget(weakWidth, weakHeight, GL_RGBA16F, GL_RGBA, GL_FLOAT,
                                  bloomWeakFramebuffer_, bloomWeakTexture_);
    const bool weakPingPong = CreateTarget(weakWidth, weakHeight, GL_RGBA16F, GL_RGBA, GL_FLOAT,
                                           bloomWeakPingPongFramebuffer_, bloomWeakPingPongTexture_);
    const bool toneMapped = CreateTarget(width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
                                         toneMappedFramebuffer_, toneMappedTexture_);
    width_ = width;
    height_ = height;
    if (scene && strong && strongPingPong && weak && weakPingPong && toneMapped && glGetError() == GL_NO_ERROR) return true;
    Destroy();
    return false;
}

std::uint32_t OpenGLRenderTargets::SceneFramebuffer() const noexcept { return sceneFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomStrongFramebuffer() const noexcept { return bloomStrongFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomPingPongFramebuffer() const noexcept { return bloomPingPongFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomWeakFramebuffer() const noexcept { return bloomWeakFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomWeakPingPongFramebuffer() const noexcept { return bloomWeakPingPongFramebuffer_; }
std::uint32_t OpenGLRenderTargets::ToneMappedFramebuffer() const noexcept { return toneMappedFramebuffer_; }
std::uint32_t OpenGLRenderTargets::SceneTexture() const noexcept { return sceneTexture_; }
std::uint32_t OpenGLRenderTargets::BloomStrongTexture() const noexcept { return bloomStrongTexture_; }
std::uint32_t OpenGLRenderTargets::BloomPingPongTexture() const noexcept { return bloomPingPongTexture_; }
std::uint32_t OpenGLRenderTargets::BloomWeakTexture() const noexcept { return bloomWeakTexture_; }
std::uint32_t OpenGLRenderTargets::BloomWeakPingPongTexture() const noexcept { return bloomWeakPingPongTexture_; }
std::uint32_t OpenGLRenderTargets::ToneMappedTexture() const noexcept { return toneMappedTexture_; }
std::uint32_t OpenGLRenderTargets::Width() const noexcept { return width_; }
std::uint32_t OpenGLRenderTargets::Height() const noexcept { return height_; }

} // namespace ParticleSaturn::Gpu::OpenGL41
