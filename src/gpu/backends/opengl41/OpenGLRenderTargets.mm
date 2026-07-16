#import <OpenGL/gl3.h>

#include "OpenGLRenderTargets.h"

#include <algorithm>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

bool CreateTarget(std::uint32_t width, std::uint32_t height, std::uint32_t& framebuffer) {
    GLuint texture = 0;
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
    const bool scene = CreateTarget(width, height, sceneFramebuffer_);
    const bool strong = CreateTarget(std::max(1U, width / 6U), std::max(1U, height / 6U), bloomStrongFramebuffer_);
    const bool weak = CreateTarget(std::max(1U, width / 12U), std::max(1U, height / 12U), bloomWeakFramebuffer_);
    return scene && strong && weak;
}

std::uint32_t OpenGLRenderTargets::SceneFramebuffer() const noexcept { return sceneFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomStrongFramebuffer() const noexcept { return bloomStrongFramebuffer_; }
std::uint32_t OpenGLRenderTargets::BloomWeakFramebuffer() const noexcept { return bloomWeakFramebuffer_; }

} // namespace ParticleSaturn::Gpu::OpenGL41
