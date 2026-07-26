#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLSevenSegmentFps {
public:
    ~OpenGLSevenSegmentFps();
    bool Initialize(const char* shaderDirectory);
    bool Render(std::uint32_t framebuffer, std::uint32_t width, std::uint32_t height,
                std::uint32_t framesPerSecond) const;

private:
    std::uint32_t program_ = 0;
    std::uint32_t vertexArray_ = 0;
    // uniform 位置在 Initialize 解析一次，绘制时不再按字符串查找（AUDIT P2-8）。
    int framesPerSecondLocation_ = -1;
    int outputSizeLocation_ = -1;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
