#pragma once

#include <cstdint>

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLParticleSystem {
public:
    static constexpr std::uint32_t ParticleCount = 1200000;
    bool Initialize(const char* transformFeedbackVertexShader);
    void Simulate(float deltaTime, float handScale, bool handTracked);
    std::uint32_t RenderVertexArray() const noexcept;
    std::uint32_t IndirectBuffer() const noexcept;
    void DrawIndirect() const;

private:
    std::uint32_t program_ = 0;
    std::uint32_t buffers_[3]{};
    std::uint32_t vertexArrays_[3]{};
    std::uint32_t transformFeedback_ = 0;
    std::uint32_t indirectBuffer_ = 0;
    std::uint32_t renderIndex_ = 0;
    std::uint32_t readIndex_ = 1;
    std::uint32_t writeIndex_ = 2;
};

} // namespace ParticleSaturn::Gpu::OpenGL41
