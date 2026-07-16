#pragma once

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets;

class OpenGLBloom {
public:
    bool Initialize(const char* shaderDirectory);
    bool Apply(const OpenGLRenderTargets& targets) const;

private:
    unsigned int downsampleProgram_ = 0;
    unsigned int blurProgram_ = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
