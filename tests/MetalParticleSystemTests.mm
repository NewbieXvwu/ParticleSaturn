#include "gpu/backends/metal/MetalBackend.h"

#include <cassert>
#include <cmath>
#include <filesystem>

#import <Metal/Metal.h>

namespace {

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.002f);
}

void VerifyDiligentToneMapping(ParticleSaturn::Gpu::Metal::MetalDevice& device, const char* libraryPath) {
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                            width:2
                                                                                           height:2
                                                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> scene = [nativeDevice newTextureWithDescriptor:descriptor];
    id<MTLTexture> bloom = [nativeDevice newTextureWithDescriptor:descriptor];
    id<MTLTexture> output = [nativeDevice newTextureWithDescriptor:descriptor];
    assert(scene != nil && bloom != nil && output != nil);

    const float scenePixels[16] = {
        2.0f, 0.5f, 0.1f, 1.0f, 2.0f, 0.5f, 0.1f, 1.0f,
        2.0f, 0.5f, 0.1f, 1.0f, 2.0f, 0.5f, 0.1f, 1.0f,
    };
    const float bloomPixels[16] = {
        0.5f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f,
        0.5f, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f,
    };
    const MTLRegion region = MTLRegionMake2D(0, 0, 2, 2);
    [scene replaceRegion:region mipmapLevel:0 withBytes:scenePixels bytesPerRow:2 * 4 * sizeof(float)];
    [bloom replaceRegion:region mipmapLevel:0 withBytes:bloomPixels bytesPerRow:2 * 4 * sizeof(float)];

    ParticleSaturn::Gpu::Metal::MetalToneMapper toneMapper;
    assert(toneMapper.Apply(device, libraryPath, scene, bloom, output, 2, 2, 0.5f));
    float pixels[16]{};
    [output getBytes:pixels bytesPerRow:2 * 4 * sizeof(float) fromRegion:region mipmapLevel:0];

    // Diligent: color = scene + bloom * 0.5, then blend 50% with color/(color+1) for HDR pixels.
    AssertNear(pixels[0], 0.5f * (2.25f + 2.25f / 3.25f));
    AssertNear(pixels[1], 0.5f * (0.75f + 0.75f / 1.75f));
    AssertNear(pixels[2], 0.5f * (0.35f + 0.35f / 1.35f));
    [scene release];
    [bloom release];
    [output release];
}

} // namespace

int main(int argc, char* argv[]) {
    assert(argc == 2);
    ParticleSaturn::Gpu::Metal::MetalDevice device;
    assert(device.Initialize());
    ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
    assert(particles.Initialize(device, argv[1], 0x53415455U));
    assert(particles.Simulate(1.0f / 120.0f, 1.0f, false, ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleCount));
    assert(particles.RenderBuffer() != nullptr);
    ParticleSaturn::Gpu::Metal::MetalStarField stars;
    assert(stars.Initialize(device, argv[1], 0x53544152U));
    assert(stars.Buffer() != nullptr);
    ParticleSaturn::Gpu::Metal::MetalParticleRenderer particleRenderer;
    assert(particleRenderer.Initialize(device, argv[1]));
    ParticleSaturn::Gpu::Metal::MetalRenderTargets targets;
    assert(targets.Create(device, 320, 180));
    assert(targets.SceneHdr() != nullptr);
    assert(targets.BloomStrong() != nullptr);
    assert(targets.BloomPingPong() != nullptr);
    assert(targets.BloomWeak() != nullptr);
    assert(targets.UiScene() != nullptr);
    assert(targets.UiOverlay() != nullptr);
    assert(targets.UiBlur() != nullptr);
    assert(targets.Composite() != nullptr);
    ParticleSaturn::Gpu::Metal::MetalToneMapper toneMapper;
    ParticleSaturn::Gpu::Metal::MetalBloom bloom;
    assert(bloom.Apply(device, argv[1], targets.SceneHdr(), targets.BloomStrong(), targets.BloomPingPong(), 320, 180, 2.0f));
    assert(toneMapper.Apply(device, argv[1], targets.SceneHdr(), targets.BloomPingPong(), targets.UiScene(), 320, 180, 0.5f));
    VerifyDiligentToneMapping(device, argv[1]);
    ParticleSaturn::Gpu::Metal::MetalAcrylic acrylic;
    assert(acrylic.Apply(device, argv[1], targets.UiScene(), targets.UiBlur(), targets.Composite(), targets.UiOverlay(),
                         320, 180, 3.0f));
    ParticleSaturn::Gpu::Metal::MetalSevenSegmentFps fps;
    assert(fps.Render(device, argv[1], targets.UiScene(), 320, 180, 120));
    ParticleSaturn::Gpu::Metal::MetalIndirectDraw indirect;
    assert(indirect.Create(device, ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleCount));
    assert(indirect.Buffer() != nullptr);
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
