#pragma once

#include <cstdint>
#include <vector>

#include "ParticleAbi.h"

namespace ParticleSaturn::Gpu::OpenGL41 {

class OpenGLParticleSystem {
  public:
    enum class SimulationMode : std::uint8_t {
        TransformFeedback,
        Analytic
    };
    static constexpr std::uint32_t ParticleCount = 1200000;
    using ParticleSnapshot                       = ShaderAbi::Particle;

    OpenGLParticleSystem() = default;
    ~OpenGLParticleSystem();
    OpenGLParticleSystem(const OpenGLParticleSystem&)            = delete;
    OpenGLParticleSystem& operator=(const OpenGLParticleSystem&) = delete;

    bool           Initialize(const char* transformFeedbackVertexShader, const char* renderVertexShader,
                              const char* renderFragmentShader, std::uint32_t seed = 0x53415455U);
    void           Simulate(float deltaTime, float handScale, bool handTracked);
    void           SetSimulationMode(SimulationMode mode) noexcept;
    SimulationMode GetSimulationMode() const noexcept;
    bool           ReadBack(std::vector<ParticleSnapshot>& particles, std::uint32_t count) const;
    std::uint32_t  RenderVertexArray() const noexcept;
    std::uint32_t  IndirectBuffer() const noexcept;
    void           DrawIndirect(float timeSeconds = 0.0f, std::uint32_t width = 1920, std::uint32_t height = 1080,
                                float scale = 1.0f, float rotationX = 0.0f, float rotationY = 0.0f, float pixelRatio = 1.0f,
                                float densityCompensation = 1.0f, std::uint32_t particleCount = ParticleCount) const;

  private:
    std::uint32_t  program_       = 0;
    std::uint32_t  renderProgram_ = 0;
    std::uint32_t  buffers_[3]{};
    std::uint32_t  vertexArrays_[3]{};
    std::uint32_t  transformFeedback_   = 0;
    std::uint32_t  analyticBuffer_      = 0;
    std::uint32_t  analyticVertexArray_ = 0;
    std::uint32_t  indirectBuffer_      = 0;
    std::uint32_t  renderIndex_         = 0;
    std::uint32_t  readIndex_           = 1;
    std::uint32_t  writeIndex_          = 2;
    SimulationMode simulationMode_      = SimulationMode::TransformFeedback;
    float          analyticPhase_       = 0.0f;
    // uniform 位置在 Initialize 解析一次，模拟/绘制时不再按字符串查找（AUDIT P2-8）。
    int                            deltaTimeLocation_   = -1;
    int                            handScaleLocation_   = -1;
    int                            handTrackedLocation_ = -1;
    static constexpr std::uint32_t RenderUniformCount   = 9;
    int                            renderUniformLocations_[RenderUniformCount]{};
};

} // namespace ParticleSaturn::Gpu::OpenGL41
