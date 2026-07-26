#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLRenderTargets;

class OpenGLBloom {
public:
    bool Initialize(const char* shaderDirectory);
    bool Apply(const OpenGLRenderTargets& targets, float blurStrength = 2.0f) const;
    bool ApplyUiBlur(const OpenGLRenderTargets& targets, float blurStrength = 2.0f) const;

private:
    // uniform 位置在 Initialize 解析一次，绘制时不再按字符串查找（AUDIT P2-8）。
    struct KawaseUniforms {
        int source = -1;
        int texelSize = -1;
        int offset = -1;
        int threshold = -1;
    };
    struct AcrylicUniforms {
        int source = -1;
        int tint = -1;
        int baseOpacity = -1;
        int saturation = -1;
        int adaptive = -1;
        int darkMode = -1;
        int exclusion = -1;
    };

    void DrawPass(unsigned int program, const KawaseUniforms& uniforms, unsigned int sourceTexture,
                  unsigned int targetFramebuffer, std::uint32_t targetWidth, std::uint32_t targetHeight,
                  std::uint32_t sourceWidth, std::uint32_t sourceHeight, float offset, float threshold) const;
    void CompositePass(unsigned int sourceTexture, unsigned int targetFramebuffer, std::uint32_t width,
                       std::uint32_t height, bool weak) const;

    unsigned int downsampleProgram_ = 0;
    unsigned int blurProgram_ = 0;
    unsigned int acrylicProgram_ = 0;
    KawaseUniforms downsampleUniforms_{};
    KawaseUniforms blurUniforms_{};
    AcrylicUniforms acrylicUniforms_{};
};

} // namespace ParticleSaturn::Gpu::OpenGL41
