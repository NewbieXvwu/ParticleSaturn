#pragma once

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets;

class OpenGLToneMapper {
public:
    bool Initialize(const char* shaderDirectory);
    bool Apply(const OpenGLRenderTargets& targets) const;

private:
    unsigned int program_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
