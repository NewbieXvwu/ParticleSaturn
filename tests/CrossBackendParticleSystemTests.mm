#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "gpu/backends/metal/MetalBackend.h"
#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr std::uint32_t Seed = 0x53415455U;
constexpr std::uint32_t SampleCount = 64U;

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.002f);
}

void CompareSnapshots(const std::vector<ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleSnapshot>& metal,
                      const std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot>& openGl) {
    assert(metal.size() == SampleCount && openGl.size() == SampleCount);
    for (std::size_t index = 0; index < metal.size(); ++index) {
        for (std::size_t component = 0; component < 4; ++component) {
            AssertNear(metal[index].position[component], openGl[index].position[component]);
        }
        assert(metal[index].color == openGl[index].color);
        AssertNear(metal[index].speed, openGl[index].speed);
        assert(metal[index].isRing == static_cast<std::uint32_t>(openGl[index].isRing));
        assert(metal[index].padding == static_cast<std::uint32_t>(openGl[index].padding));
    }
}

void ReadAndCompare(ParticleSaturn::Gpu::Metal::MetalParticleSystem& metal,
                    ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem& openGl,
                    NSOpenGLContext* openGlContext) {
    std::vector<ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleSnapshot> metalSnapshots;
    std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> openGlSnapshots;
    assert(metal.ReadBack(metalSnapshots, SampleCount));
    [openGlContext makeCurrentContext];
    assert(openGl.ReadBack(openGlSnapshots, SampleCount));
    CompareSnapshots(metalSnapshots, openGlSnapshots);
}

} // namespace

int main(int argc, char* argv[]) {
    assert(argc == 5);
    @autoreleasepool {
        const NSOpenGLPixelFormatAttribute attributes[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFADoubleBuffer, 0,
        };
        auto* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        auto* context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        [format release];
        assert(context != nil);
        [context makeCurrentContext];

        {
            ParticleSaturn::Gpu::Metal::MetalDevice device;
            assert(device.Initialize());
            ParticleSaturn::Gpu::Metal::MetalParticleSystem metal;
            assert(metal.Initialize(device, argv[1], Seed));

            ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem openGl;
            assert(openGl.Initialize(argv[2], argv[3], argv[4], Seed));
            ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets targets;
            assert(targets.Create(1, 1));
            glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
            ReadAndCompare(metal, openGl, context);

            for (std::uint32_t frame = 0; frame < 4U; ++frame) {
                assert(metal.Simulate(1.0f / 120.0f, 1.0f, false,
                                      ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleCount));
                [context makeCurrentContext];
                glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
                openGl.Simulate(1.0f / 120.0f, 1.0f, false);
                assert(glGetError() == GL_NO_ERROR);
                ReadAndCompare(metal, openGl, context);
            }
        }
        [context release];
    }
    return 0;
}
