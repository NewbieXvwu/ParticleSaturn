#pragma once

#include "gpu/interface/GpuCapabilities.h"

namespace ParticleSaturn::Render {

enum class ParticleSimulationMode {
    Compute,
    TransformFeedback,
    Analytic,
};

struct ParticleSimulationSupport {
    bool transformFeedback = false;
    bool analytic = false;
};

ParticleSimulationMode SelectParticleSimulationMode(
    const Gpu::GpuCapabilities& capabilities,
    const ParticleSimulationSupport& support,
    bool preferAnalytic = false);

} // namespace ParticleSaturn::Render
