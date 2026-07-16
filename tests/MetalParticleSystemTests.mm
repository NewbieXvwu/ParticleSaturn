#include "gpu/backends/metal/MetalBackend.h"

#include <cassert>

int main(int argc, char* argv[]) {
    assert(argc == 2);
    ParticleSaturn::Gpu::Metal::MetalDevice device;
    assert(device.Initialize());
    ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
    assert(particles.Initialize(device, argv[1], 0x53415455U));
    assert(particles.Simulate(1.0f / 120.0f, 1.0f, false));
    assert(particles.RenderBuffer() != nullptr);
    return 0;
}
