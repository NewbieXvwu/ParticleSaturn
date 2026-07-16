#include "gpu/backends/metal/MetalBackend.h"

#include <cassert>
#include <filesystem>

int main(int argc, char* argv[]) {
    assert(argc == 2);
    ParticleSaturn::Gpu::Metal::MetalDevice device;
    assert(device.Initialize());
    ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
    assert(particles.Initialize(device, argv[1], 0x53415455U));
    assert(particles.Simulate(1.0f / 120.0f, 1.0f, false));
    assert(particles.RenderBuffer() != nullptr);
    ParticleSaturn::Gpu::Metal::MetalStarField stars;
    assert(stars.Initialize(device, argv[1], 0x53544152U));
    assert(stars.Buffer() != nullptr);
    ParticleSaturn::Gpu::Metal::MetalRenderTargets targets;
    assert(targets.Create(device, 1920, 1080));
    assert(targets.SceneHdr() != nullptr);
    assert(targets.BloomStrong() != nullptr);
    assert(targets.BloomWeak() != nullptr);
    const auto cachePath = std::filesystem::temp_directory_path() / "ParticleSaturnTests.metallibarchive";
    std::filesystem::remove(cachePath);
    ParticleSaturn::Gpu::Metal::MetalPipelineCache cache;
    assert(cache.Load(device, cachePath.string()));
    assert(cache.AddComputeFunction(device, argv[1], "InitializeParticles"));
    assert(cache.Save(cachePath.string()));
    assert(std::filesystem::is_regular_file(cachePath));
    std::filesystem::remove(cachePath);
    return 0;
}
