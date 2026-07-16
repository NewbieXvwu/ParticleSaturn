#include "ParticleSimulationStrategy.h"

#include <stdexcept>

namespace ParticleSaturn::Render {

ParticleSimulationMode SelectParticleSimulationMode(
    const Gpu::GpuCapabilities& capabilities,
    const ParticleSimulationSupport& support,
    bool preferAnalytic) {
    if (capabilities.supportsCompute && capabilities.supportsStorageBuffer) {
        return ParticleSimulationMode::Compute;
    }
    if (preferAnalytic && support.analytic) {
        return ParticleSimulationMode::Analytic;
    }
    if (support.transformFeedback) {
        return ParticleSimulationMode::TransformFeedback;
    }
    if (support.analytic) {
        return ParticleSimulationMode::Analytic;
    }
    throw std::runtime_error{"no supported particle simulation strategy"};
}

} // namespace ParticleSaturn::Render
