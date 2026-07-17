#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLSevenSegmentFps.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

void AssertNear(float actual, float expected) {
    assert(std::abs(actual - expected) < 0.0001f);
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

void UnpackColor(std::uint32_t color, float& red, float& green, float& blue) {
    red = static_cast<float>((color >> 16U) & 0xffU) / 255.0f;
    green = static_cast<float>((color >> 8U) & 0xffU) / 255.0f;
    blue = static_cast<float>(color & 0xffU) / 255.0f;
}

std::uint32_t PackColor(float red, float green, float blue, float alpha) {
    const auto pack = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return pack(red) | (pack(green) << 8U) | (pack(blue) << 16U) | (pack(alpha) << 24U);
}

ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot ExpectedDiligentParticle(std::uint32_t id,
                                                                                                   std::uint32_t seed) {
    using Snapshot = ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot;
    constexpr float radius = 18.0f;
    std::uint32_t rng = id * 1973U + seed * 9277U + 26699U;
    Snapshot particle{};
    float red = 1.0f, green = 1.0f, blue = 1.0f, alpha = 1.0f;
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
            size = 0.5f; alpha = 0.3f;
        } else if (zone < 0.65f) {
            const float mix = Random01(rng);
            ringRadius = radius * (1.525f + mix * 0.425f);
            red = (205.0f + 15.0f * mix) / 255.0f;
            green = (191.0f + 12.0f * mix) / 255.0f;
            blue = (160.0f + 26.0f * mix) / 255.0f;
            size = 0.8f + Random01(rng) * 0.6f;
            alpha = std::sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f;
        } else if (zone < 0.69f) {
            ringRadius = radius * (1.95f + Random01(rng) * 0.075f);
            UnpackColor(0x050505U, red, green, blue);
            size = 0.3f; alpha = 0.1f;
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
        particle.isRing = 1U;
    }
    particle.color = PackColor(red, green, blue, alpha);
    return particle;
}

void VerifyInitializedParticles(ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem& particles) {
    std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> snapshots;
    assert(particles.ReadBack(snapshots, 64));
    assert(snapshots.size() == 64);
    using Snapshot = ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot;
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const auto& actual = snapshots[index];
        const Snapshot expected = ExpectedDiligentParticle(static_cast<std::uint32_t>(index), 0x53415455U);
        for (std::size_t component = 0; component < 4; ++component) AssertNear(actual.position[component], expected.position[component]);
        assert(actual.color == expected.color);
        AssertNear(actual.speed, expected.speed);
        assert(actual.isRing == expected.isRing);
        assert(actual.padding == expected.padding);
    }
    for (const auto& particle : snapshots) {
        assert(std::isfinite(particle.position[0]));
        assert(std::isfinite(particle.position[1]));
        assert(std::isfinite(particle.position[2]));
        assert(particle.position[3] > 0.0f);
        assert(particle.color != 0U);
        assert(particle.isRing == 0U || particle.isRing == 1U);
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
        struct DrawArraysIndirectCommand {
            std::uint32_t count;
            std::uint32_t instanceCount;
            std::uint32_t first;
            std::uint32_t baseInstance;
        } indirect{};
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, particles.IndirectBuffer());
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(indirect), &indirect);
        assert(indirect.count == 6);
        assert(indirect.instanceCount == ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleCount);
        assert(indirect.first == 0);
        assert(indirect.baseInstance == 0);
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
            assert(particle.isRing == 0U || particle.isRing == 1U);
            assert(particle.padding == 0U);
        }
        particles.SetSimulationMode(ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::Analytic);
        particles.Simulate(2.0f / 120.0f, 1.0f, false);
        std::vector<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::ParticleSnapshot> analyticFrame;
        assert(particles.ReadBack(analyticFrame, 1));
        const auto initial = ExpectedDiligentParticle(0, 0x53415455U);
        const float angle = (initial.isRing == 0U ? 0.03f : initial.speed * 0.2f) * (2.0f / 120.0f);
        AssertNear(analyticFrame[0].position[0], initial.position[0] * std::cos(angle) - initial.position[2] * std::sin(angle));
        AssertNear(analyticFrame[0].position[2], initial.position[0] * std::sin(angle) + initial.position[2] * std::cos(angle));
        particles.SetSimulationMode(ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::TransformFeedback);
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
        const float sceneBeforeUi[4] = {color[0], color[1], color[2], color[3]};
        assert(bloom.ApplyUiBlur(targets, 2.0f));
        float acrylic[4]{};
        glBindFramebuffer(GL_FRAMEBUFFER, targets.BloomStrongFramebuffer());
        glReadPixels(160, 90, 1, 1, GL_RGBA, GL_FLOAT, acrylic);
        assert(glGetError() == GL_NO_ERROR);
        assert(acrylic[0] > 0.0f && acrylic[1] > 0.0f && acrylic[2] > 0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
        glReadPixels(960, 540, 1, 1, GL_RGBA, GL_FLOAT, color);
        for (std::size_t channel = 0; channel < 4; ++channel) AssertNear(color[channel], sceneBeforeUi[channel]);

        ParticleSaturn::Gpu::OpenGL41::OpenGLSevenSegmentFps fps;
        assert(fps.Initialize(argv[4]));
        assert(fps.Render(targets.ToneMappedFramebuffer(), 1920, 1080, 29));
        glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
        glReadPixels(1860, 1075, 1, 1, GL_RGBA, GL_FLOAT, color);
        assert(color[0] > 0.99f && color[1] < 0.3f);
        glReadPixels(1830, 1057, 1, 1, GL_RGBA, GL_FLOAT, color);
        assert(color[0] > 0.99f && color[1] < 0.3f);

        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glClearColor(0.1f, 0.05f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.BloomPingPongFramebuffer());
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        assert(toneMapper.Apply(targets, 0.0f, true));
        glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
        glReadPixels(960, 540, 1, 1, GL_RGBA, GL_FLOAT, color);
        assert(color[3] > 0.09f && color[3] < 0.11f);
        assert(color[0] > 0.005f && color[0] < 0.015f);
        assert(color[1] > 0.0f && color[1] < color[0]);
        [context release];
    }
    return 0;
}
