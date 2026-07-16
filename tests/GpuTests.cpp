#include "gpu/interface/GpuCapabilities.h"
#include "gpu/interface/GpuTypes.h"
#include "render/passes/ParticleSimulationStrategy.h"

#include <cassert>
#include <stdexcept>

using namespace ParticleSaturn;

int main() {
    const auto usage = Gpu::BufferUsage::Vertex | Gpu::BufferUsage::Indirect;
    assert(Gpu::HasUsage(usage, Gpu::BufferUsage::Vertex));
    assert(Gpu::HasUsage(usage, Gpu::BufferUsage::Indirect));
    assert(!Gpu::HasUsage(usage, Gpu::BufferUsage::Storage));

    Gpu::GpuCapabilities compute{};
    compute.supportsCompute = true;
    compute.supportsStorageBuffer = true;
    assert(Render::SelectParticleSimulationMode(compute, {}) == Render::ParticleSimulationMode::Compute);

    const Render::ParticleSimulationSupport gl41{true, true};
    assert(Render::SelectParticleSimulationMode({}, gl41) == Render::ParticleSimulationMode::TransformFeedback);
    assert(Render::SelectParticleSimulationMode({}, gl41, true) == Render::ParticleSimulationMode::Analytic);

    bool failed = false;
    try {
        static_cast<void>(Render::SelectParticleSimulationMode({}, {}));
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed);
    return 0;
}
