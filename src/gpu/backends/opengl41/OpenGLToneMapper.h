#pragma once

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets;

class OpenGLToneMapper {
public:
    bool Initialize(const char* shaderDirectory);
    bool Apply(const OpenGLRenderTargets& targets, float bloomStrength = 0.5f, bool transparent = false) const;
    bool Present(const OpenGLRenderTargets& targets, bool transparent) const;

private:
    unsigned int program_ = 0;
    unsigned int presentProgram_ = 0;
    // uniform 位置在 Initialize 解析一次，绘制时不再按字符串查找（AUDIT P2-8）。
    int sceneLocation_ = -1;
    int bloomLocation_ = -1;
    int bloomStrengthLocation_ = -1;
    int transparentLocation_ = -1;
    int presentSceneLocation_ = -1;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
