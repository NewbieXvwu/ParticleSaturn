#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.0001f);
}

void VerifyInitializedParticles(ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem& particles) {
    std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> snapshots;
    assert(particles.ReadBack(snapshots, 64));
    assert(snapshots.size() == 64);
    using Snapshot = ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot;
    const Snapshot expected[] = {
        {{7.55536461f, 8.98992634f, 12.9282703f, 1.78891587f}, 0xccc5dae3U, 0.0f, 0.0f, 0.0f},
        {{-1.69500279f, -16.1162949f, -0.682979405f, 1.37864661f}, 0xcc70a0c9U, 0.0f, 0.0f, 0.0f},
        {{15.9242182f, -0.907382071f, 8.3308363f, 1.67867303f}, 0xccc5dae3U, 0.0f, 0.0f, 0.0f},
        {{3.47033906f, -0.0189455301f, -27.8411579f, 1.26095307f}, 0xd9a2c0ceU, 1.51033187f, 1.0f, 0.0f},
    };
    for (std::size_t index = 0; index < std::size(expected); ++index) {
        const auto& actual = snapshots[index];
        for (std::size_t component = 0; component < 4; ++component) AssertNear(actual.position[component], expected[index].position[component]);
        assert(actual.color == expected[index].color);
        AssertNear(actual.speed, expected[index].speed);
        AssertNear(actual.isRing, expected[index].isRing);
        AssertNear(actual.padding, expected[index].padding);
    }
    for (const auto& particle : snapshots) {
        assert(std::isfinite(particle.position[0]));
        assert(std::isfinite(particle.position[1]));
        assert(std::isfinite(particle.position[2]));
        assert(particle.position[3] > 0.0f);
        assert(particle.color != 0U);
        assert(particle.isRing == 0.0f || particle.isRing == 1.0f);
    }
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
        ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem particles;
        assert(particles.Initialize(argv[1], argv[2], argv[3]));
        VerifyInitializedParticles(particles);
        ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets targets;
        assert(targets.Create(1920, 1080));
        assert(targets.SceneFramebuffer() != 0);
        assert(targets.BloomStrongFramebuffer() != 0);
        assert(targets.BloomWeakFramebuffer() != 0);
        assert(targets.ToneMappedFramebuffer() != 0);
        assert(targets.SceneTexture() != 0);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glViewport(0, 0, 1920, 1080);
        particles.Simulate(1.0f / 120.0f, 1.0f, false);
        assert(glGetError() == GL_NO_ERROR);
        assert(particles.RenderVertexArray() != 0);
        assert(particles.IndirectBuffer() != 0);
        std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> firstFrame;
        assert(particles.ReadBack(firstFrame, 1));
        particles.Simulate(1.0f / 120.0f, 1.0f, false);
        std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> secondFrame;
        assert(particles.ReadBack(secondFrame, 64));
        AssertNear(secondFrame[0].position[1], firstFrame[0].position[1]);
        assert(std::abs(secondFrame[0].position[0] - firstFrame[0].position[0]) > 0.000001f ||
               std::abs(secondFrame[0].position[2] - firstFrame[0].position[2]) > 0.000001f);
        for (const auto& particle : secondFrame) {
            for (float position : particle.position) assert(std::isfinite(position));
            assert(particle.color != 0U);
            assert(particle.isRing == 0.0f || particle.isRing == 1.0f);
            AssertNear(particle.padding, 0.0f);
        }
        particles.DrawIndirect();
        assert(glGetError() == GL_NO_ERROR);
        glClearColor(3.0f, 2.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        assert(glGetError() == GL_NO_ERROR);
        ParticleSaturn::Gpu::OpenGL41::OpenGLBloom bloom;
        assert(bloom.Initialize(argv[4]));
        assert(bloom.Apply(targets));
        float bloomColor[4]{};
        glBindFramebuffer(GL_FRAMEBUFFER, targets.BloomPingPongFramebuffer());
        glReadPixels(160, 90, 1, 1, GL_RGBA, GL_FLOAT, bloomColor);
        assert(glGetError() == GL_NO_ERROR);
        assert(bloomColor[0] > 0.1f && bloomColor[1] > 0.1f && bloomColor[2] > 0.1f);
        ParticleSaturn::Gpu::OpenGL41::OpenGLToneMapper toneMapper;
        assert(toneMapper.Initialize(argv[4]));
        assert(toneMapper.Apply(targets));
        float color[4]{};
        glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
        glReadPixels(960, 540, 1, 1, GL_RGBA, GL_FLOAT, color);
        assert(glGetError() == GL_NO_ERROR);
        assert(color[0] > 0.5f && color[0] <= 1.0f);
        assert(color[1] > 0.5f && color[1] <= 1.0f);
        assert(color[2] > 0.5f && color[2] <= 1.0f);
        [context release];
    }
    return 0;
}
