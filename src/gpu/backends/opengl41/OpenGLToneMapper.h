#pragma once

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets;

class OpenGLToneMapper {
  public:
    bool Initialize(const char* shaderDirectory);
    bool Apply(const OpenGLRenderTargets& targets, float bloomStrength = 0.5f, bool transparent = false) const;
    bool Present(const OpenGLRenderTargets& targets, bool transparent) const;

  private:
    unsigned int program_        = 0;
    unsigned int presentProgram_ = 0;
    // uniform 位置在 Initialize 解析一次，绘制时不再按字符串查找（AUDIT P2-8）。
    // tonemap 程序来自单源翻译（D-004）：常量走 std140 UBO，纹理为组合采样器。
    int          sceneLocation_        = -1;
    int          bloomLocation_        = -1;
    int          presentSceneLocation_ = -1;
    unsigned int constantsBuffer_      = 0;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
