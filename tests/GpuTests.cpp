#include "gpu/interface/GpuCapabilities.h"
#include "gpu/interface/GpuTypes.h"

#include <cassert>

using namespace ParticleSaturn;

int main() {
    const auto usage = Gpu::BufferUsage::Vertex | Gpu::BufferUsage::Indirect;
    assert(Gpu::HasUsage(usage, Gpu::BufferUsage::Vertex));
    assert(Gpu::HasUsage(usage, Gpu::BufferUsage::Indirect));
    assert(!Gpu::HasUsage(usage, Gpu::BufferUsage::Storage));
    return 0;
}
