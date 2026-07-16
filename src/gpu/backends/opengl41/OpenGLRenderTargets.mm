#import <OpenGL/gl3.h>

#include "OpenGLRenderTargets.h"

#include <algorithm>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

bool CreateTarget(std::uint32_t width, std::uint32_t height, std::uint32_t& framebuffer, std::uint32_t& texture) {
    glGenFramebuffers(1, &framebuffer);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

} // namespace

bool OpenGLRenderTargets::Create(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return false;
    const bool scene = CreateTarget(width, height, sceneFramebuffer_, sceneTexture_);
    const bool strong = CreateTarget(std::max(1U, width / 6U), std::max(1U, height / 6U), bloomStrongFramebuffer_, bloomStrongTexture_);
    const bool weak = CreateTarget(std::max(1U, width / 12U), std::max(1U, height / 12U), bloomWeakFramebuffer_, bloomWeakTexture_);
    width_ = width;
    height_ = height;
    return scene && strong && weak;
}

std::uint32_t OpenGLRenderTargets::SceneFramebuffer() const noexcept { return sceneFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomStrongFramebuffer() const noexcept { return bloomStrongFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomWeakFramebuffer() const noexcept { return bloomWeakFramebuffer_; }
std::uint32_t OpenGLRenderTargets::SceneTexture() const noexcept { return sceneTexture_; }
std::uint32_t OpenGLRenderTargets::BloomStrongTexture() const noexcept { return bloomStrongTexture_; }
std::uint32_t OpenGLRenderTargets::BloomWeakTexture() const noexcept { return bloomWeakTexture_; }
std::uint32_t OpenGLRenderTargets::Width() const noexcept { return width_; }
std::uint32_t OpenGLRenderTargets::Height() const noexcept { return height_; }

} // namespace ParticleSaturn::Gpu::OpenGL41
