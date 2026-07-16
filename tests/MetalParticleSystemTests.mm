#include "gpu/backends/metal/MetalBackend.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#import <Metal/Metal.h>

namespace {

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.002f);
}

std::uint32_t PcgHash(std::uint32_t input) {
    const std::uint32_t state = input * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

float Random01(std::uint32_t& state) {
    state = PcgHash(state);
    return static_cast<float>(state) * (1.0f / 4294967296.0f);
}

std::uint32_t PackColor(float red, float green, float blue, float alpha) {
    const auto pack = [](float value) { return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return pack(red) | (pack(green) << 8U) | (pack(blue) << 16U) | (pack(alpha) << 24U);
}

void UnpackColor(std::uint32_t color, float& red, float& green, float& blue) {
    red = static_cast<float>((color >> 16U) & 0xffU) / 255.0f;
    green = static_cast<float>((color >> 8U) & 0xffU) / 255.0f;
    blue = static_cast<float>(color & 0xffU) / 255.0f;
}

ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleSnapshot InitializeDiligentParticle(std::uint32_t id, std::uint32_t seed) {
    using Snapshot = ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleSnapshot;
    constexpr float radius = 18.0f;
    std::uint32_t rng = id * 1973U + seed * 9277U + 26699U;
    Snapshot particle{};
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    float alpha = 1.0f;
    if (Random01(rng) < 0.25f) {
        const float theta = 6.28318f * Random01(rng);
        const float phi = std::acos(2.0f * Random01(rng) - 1.0f);
        particle.position[0] = radius * std::sin(phi) * std::cos(theta);
        particle.position[1] = radius * std::cos(phi) * 0.9f;
        particle.position[2] = radius * std::sin(phi) * std::sin(theta);
        const float latitude = (particle.position[1] / (0.9f * radius) + 1.0f) * 0.5f;
        const int paletteIndex = static_cast<int>(latitude * 4.0f + std::cos(latitude * 40.0f) * 0.8f +
                                                  std::cos(latitude * 15.0f) * 0.4f);
        constexpr std::uint32_t palette[4] = {0xE3DAC5U, 0xC9A070U, 0xE3DAC5U, 0xB08D55U};
        UnpackColor(palette[(paletteIndex % 4 + 4) % 4], red, green, blue);
        particle.position[3] = 1.0f + Random01(rng) * 0.8f;
        alpha = 0.8f;
    } else {
        const float zone = Random01(rng);
        float ringRadius = 0.0f;
        float size = 1.0f;
        if (zone < 0.15f) {
            ringRadius = radius * (1.235f + Random01(rng) * 0.29f);
            UnpackColor(0x2A2520U, red, green, blue);
            size = 0.5f;
            alpha = 0.3f;
        } else if (zone < 0.65f) {
            const float mix = Random01(rng);
            ringRadius = radius * (1.525f + mix * 0.425f);
            red = (205.0f + (220.0f - 205.0f) * mix) / 255.0f;
            green = (191.0f + (203.0f - 191.0f) * mix) / 255.0f;
            blue = (160.0f + (186.0f - 160.0f) * mix) / 255.0f;
            size = 0.8f + Random01(rng) * 0.6f;
            alpha = std::sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f;
        } else if (zone < 0.69f) {
            ringRadius = radius * (1.95f + Random01(rng) * 0.075f);
            UnpackColor(0x050505U, red, green, blue);
            size = 0.3f;
            alpha = 0.1f;
        } else if (zone < 0.99f) {
            ringRadius = radius * (2.025f + Random01(rng) * 0.245f);
            UnpackColor(0x989085U, red, green, blue);
            size = 0.7f;
            alpha = ringRadius > radius * 2.2f && ringRadius < radius * 2.21f ? 0.1f : 0.6f;
        } else {
            ringRadius = radius * (2.32f + Random01(rng) * 0.02f);
            UnpackColor(0xAFAFA0U, red, green, blue);
            alpha = 0.7f;
        }
        const float theta = Random01(rng) * 6.28318f;
        particle.position[0] = ringRadius * std::cos(theta);
        particle.position[1] = (Random01(rng) - 0.5f) * (ringRadius > radius * 2.3f ? 0.4f : 0.15f);
        particle.position[2] = ringRadius * std::sin(theta);
        particle.position[3] = size;
        particle.speed = 8.0f / std::sqrt(ringRadius);
        particle.isRing = 1;
    }
    particle.color = PackColor(red, green, blue, alpha);
    return particle;
}

void VerifyDiligentParticleInitialization(ParticleSaturn::Gpu::Metal::MetalParticleSystem& particles) {
    using Snapshot = ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleSnapshot;
    std::vector<Snapshot> actual;
    assert(particles.ReadBack(actual, 64));
    assert(actual.size() == 64);
    for (std::uint32_t index = 0; index < actual.size(); ++index) {
        const Snapshot expected = InitializeDiligentParticle(index, 0x53415455U);
        for (std::size_t component = 0; component < 4; ++component) AssertNear(actual[index].position[component], expected.position[component]);
        AssertNear(actual[index].speed, expected.speed);
        assert(actual[index].color == expected.color);
        assert(actual[index].isRing == expected.isRing);
        assert(actual[index].padding == expected.padding);
    }
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

void VerifyDiligentFpsGeometry(ParticleSaturn::Gpu::Metal::MetalDevice& device, const char* libraryPath) {
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                            width:120
                                                                                           height:50
                                                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> output = [nativeDevice newTextureWithDescriptor:descriptor];
    assert(output != nil);
    ParticleSaturn::Gpu::Metal::MetalSevenSegmentFps fps;
    assert(fps.Render(device, libraryPath, output, 120, 50, 29));
    float pixels[120 * 50 * 4]{};
    const MTLRegion region = MTLRegionMake2D(0, 0, 120, 50);
    [output getBytes:pixels bytesPerRow:120 * 4 * sizeof(float) fromRegion:region mipmapLevel:0];
    const auto redAt = [&](std::uint32_t x, std::uint32_t y) { return pixels[(y * 120 + x) * 4]; };
    // Diligent places the units digit at width-60 and tens digit 30 pixels to its left.
    AssertNear(redAt(60, 4), 1.0f);
    AssertNear(redAt(79, 39), 1.0f);
    AssertNear(redAt(30, 4), 1.0f);
    assert(redAt(30, 22) > 0.99f);
    [output release];
}

id<MTLTexture> CreateSharedTexture(id<MTLDevice> device, std::uint32_t width, std::uint32_t height) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                            width:width
                                                                                           height:height
                                                                                        mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModeShared;
    return [device newTextureWithDescriptor:descriptor];
}

void VerifyBloomThresholdAndAcrylicIsolation(ParticleSaturn::Gpu::Metal::MetalDevice& device, const char* libraryPath) {
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    id<MTLTexture> scene = CreateSharedTexture(nativeDevice, 12, 12);
    id<MTLTexture> bloomA = CreateSharedTexture(nativeDevice, 2, 2);
    id<MTLTexture> bloomB = CreateSharedTexture(nativeDevice, 2, 2);
    assert(scene != nil && bloomA != nil && bloomB != nil);

    float darkScene[12 * 12 * 4];
    for (std::size_t index = 0; index < 12U * 12U; ++index) {
        darkScene[index * 4U + 0U] = 0.5f;
        darkScene[index * 4U + 1U] = 0.5f;
        darkScene[index * 4U + 2U] = 0.5f;
        darkScene[index * 4U + 3U] = 1.0f;
    }
    const MTLRegion sceneRegion = MTLRegionMake2D(0, 0, 12, 12);
    [scene replaceRegion:sceneRegion mipmapLevel:0 withBytes:darkScene bytesPerRow:12 * 4 * sizeof(float)];
    ParticleSaturn::Gpu::Metal::MetalBloom bloom;
    assert(bloom.Apply(device, libraryPath, scene, bloomA, bloomB, 12, 12, 2.0f));
    float bloomPixels[2 * 2 * 4]{};
    [bloomB getBytes:bloomPixels bytesPerRow:2 * 4 * sizeof(float) fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
    for (std::size_t pixel = 0; pixel < 4U; ++pixel) {
        AssertNear(bloomPixels[pixel * 4U + 0U], 0.0f);
        AssertNear(bloomPixels[pixel * 4U + 1U], 0.0f);
        AssertNear(bloomPixels[pixel * 4U + 2U], 0.0f);
        AssertNear(bloomPixels[pixel * 4U + 3U], 1.0f);
    }

    id<MTLTexture> blurA = CreateSharedTexture(nativeDevice, 2, 2);
    id<MTLTexture> blurB = CreateSharedTexture(nativeDevice, 2, 2);
    id<MTLTexture> blurWeakA = CreateSharedTexture(nativeDevice, 1, 1);
    id<MTLTexture> blurWeakB = CreateSharedTexture(nativeDevice, 1, 1);
    id<MTLTexture> acrylic = CreateSharedTexture(nativeDevice, 2, 2);
    id<MTLTexture> acrylicWeak = CreateSharedTexture(nativeDevice, 1, 1);
    assert(blurA != nil && blurB != nil && blurWeakA != nil && blurWeakB != nil && acrylic != nil && acrylicWeak != nil);
    float before[12 * 12 * 4]{};
    [scene getBytes:before bytesPerRow:12 * 4 * sizeof(float) fromRegion:sceneRegion mipmapLevel:0];
    ParticleSaturn::Gpu::Metal::MetalAcrylic compositor;
    assert(compositor.Apply(device, libraryPath, scene, blurA, blurB, blurWeakA, blurWeakB, acrylic, acrylicWeak, 12, 12, 2.0f));
    float after[12 * 12 * 4]{};
    [scene getBytes:after bytesPerRow:12 * 4 * sizeof(float) fromRegion:sceneRegion mipmapLevel:0];
    assert(std::memcmp(before, after, sizeof(before)) == 0);
    // A uniform scene intentionally has identical blur samples.  A local HDR
    // highlight proves the 1/6 and 1/12 Acrylic products run independently.
    for (std::uint32_t y = 0; y < 12U; ++y) {
        for (std::uint32_t x = 0; x < 12U; ++x) {
            const std::size_t pixel = (y * 12U + x) * 4U;
            darkScene[pixel + 0U] = static_cast<float>(x) * (4.0f / 11.0f);
            darkScene[pixel + 1U] = static_cast<float>(y) * (2.0f / 11.0f);
            darkScene[pixel + 2U] = 0.25f;
            darkScene[pixel + 3U] = 1.0f;
        }
    }
    [scene replaceRegion:sceneRegion mipmapLevel:0 withBytes:darkScene bytesPerRow:12 * 4 * sizeof(float)];
    assert(compositor.Apply(device, libraryPath, scene, blurA, blurB, blurWeakA, blurWeakB, acrylic, acrylicWeak, 12, 12, 2.0f));
    float strongPixels[2 * 2 * 4]{};
    float weakPixels[4]{};
    [acrylic getBytes:strongPixels bytesPerRow:2 * 4 * sizeof(float) fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
    [acrylicWeak getBytes:weakPixels bytesPerRow:4 * sizeof(float) fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    assert(strongPixels[0] > 0.0f && weakPixels[0] > 0.0f);
    assert(std::fabs(strongPixels[0] - weakPixels[0]) > 0.0001f);

    [scene release];
    [bloomA release];
    [bloomB release];
    [blurA release];
    [blurB release];
    [blurWeakA release];
    [blurWeakB release];
    [acrylic release];
    [acrylicWeak release];
}

} // namespace

int main(int argc, char* argv[]) {
    assert(argc == 2);
    ParticleSaturn::Gpu::Metal::MetalDevice device;
    assert(device.Initialize());
    ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
    assert(particles.Initialize(device, argv[1], 0x53415455U));
    VerifyDiligentParticleInitialization(particles);
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
    assert(targets.UiBlurWeak() != nullptr);
    assert(targets.UiBlurWeakPingPong() != nullptr);
    assert(targets.UiOverlayWeak() != nullptr);
    assert(targets.Create(device, 640, 360));
    assert(targets.SceneHdr() != nullptr);
    assert(targets.BloomPingPong() != nullptr);
    assert(targets.Create(device, 320, 180));
    ParticleSaturn::Gpu::Metal::MetalToneMapper toneMapper;
    ParticleSaturn::Gpu::Metal::MetalBloom bloom;
    assert(bloom.Apply(device, argv[1], targets.SceneHdr(), targets.BloomStrong(), targets.BloomPingPong(), 320, 180, 2.0f));
    assert(toneMapper.Apply(device, argv[1], targets.SceneHdr(), targets.BloomPingPong(), targets.UiScene(), 320, 180, 0.5f));
    VerifyDiligentToneMapping(device, argv[1]);
    VerifyDiligentFpsGeometry(device, argv[1]);
    VerifyBloomThresholdAndAcrylicIsolation(device, argv[1]);
    ParticleSaturn::Gpu::Metal::MetalAcrylic acrylic;
    assert(acrylic.Apply(device, argv[1], targets.UiScene(), targets.UiBlur(), targets.Composite(),
                         targets.UiBlurWeak(), targets.UiBlurWeakPingPong(), targets.UiOverlay(), targets.UiOverlayWeak(),
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
