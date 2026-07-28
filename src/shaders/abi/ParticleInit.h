#pragma once

// Diligent 参考粒子初始化——土星粒子云（球体 + 环带）的规范 CPU 端定义。
// 固定种子下逐位确定，是跨后端一致性比较的唯一事实来源（AUDIT P2-9）：
// GL41 生产初始化与各后端测试参照共用本实现，不得另写副本。
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ParticleAbi.h"

namespace ParticleSaturn::ShaderAbi {

inline float ParticleInitRandom01(std::uint32_t& state) {
    const std::uint32_t pcgState = state * 747796405U + 2891336453U;
    const std::uint32_t word     = ((pcgState >> ((pcgState >> 28U) + 4U)) ^ pcgState) * 277803737U;
    state                        = (word >> 22U) ^ word;
    return static_cast<float>(state) * (1.0f / 4294967296.0f);
}

inline void ParticleInitUnpackColor(std::uint32_t color, float& red, float& green, float& blue) {
    red   = static_cast<float>((color >> 16U) & 0xffU) / 255.0f;
    green = static_cast<float>((color >> 8U) & 0xffU) / 255.0f;
    blue  = static_cast<float>(color & 0xffU) / 255.0f;
}

inline std::uint32_t ParticleInitPackColor(float red, float green, float blue, float alpha) {
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(alpha) << 24U) | (channel(blue) << 16U) | (channel(green) << 8U) | channel(red);
}

inline Particle InitializeDiligentParticle(std::uint32_t id, std::uint32_t seed) {
    constexpr float radius = 18.0f;
    std::uint32_t   rng    = id * 1973U + seed * 9277U + 26699U;
    Particle        particle{};
    float           red   = 1.0f;
    float           green = 1.0f;
    float           blue  = 1.0f;
    float           alpha = 1.0f;
    if (ParticleInitRandom01(rng) < 0.25f) {
        const float theta    = 6.28318f * ParticleInitRandom01(rng);
        const float phi      = std::acos(2.0f * ParticleInitRandom01(rng) - 1.0f);
        particle.position[0] = radius * std::sin(phi) * std::cos(theta);
        particle.position[1] = radius * std::cos(phi) * 0.9f;
        particle.position[2] = radius * std::sin(phi) * std::sin(theta);
        const float latitude = (particle.position[1] / (0.9f * radius) + 1.0f) * 0.5f;
        const int   paletteIndex =
            static_cast<int>(latitude * 4.0f + std::cos(latitude * 40.0f) * 0.8f + std::cos(latitude * 15.0f) * 0.4f);
        constexpr std::uint32_t palette[4] = {0xE3DAC5U, 0xC9A070U, 0xE3DAC5U, 0xB08D55U};
        ParticleInitUnpackColor(palette[(paletteIndex % 4 + 4) % 4], red, green, blue);
        particle.position[3] = 1.0f + ParticleInitRandom01(rng) * 0.8f;
        alpha                = 0.8f;
    } else {
        const float zone       = ParticleInitRandom01(rng);
        float       ringRadius = 0.0f;
        float       size       = 1.0f;
        if (zone < 0.15f) {
            ringRadius = radius * (1.235f + ParticleInitRandom01(rng) * 0.29f);
            ParticleInitUnpackColor(0x2A2520U, red, green, blue);
            size  = 0.5f;
            alpha = 0.3f;
        } else if (zone < 0.65f) {
            const float mix = ParticleInitRandom01(rng);
            ringRadius      = radius * (1.525f + mix * 0.425f);
            red             = (205.0f + (220.0f - 205.0f) * mix) / 255.0f;
            green           = (191.0f + (203.0f - 191.0f) * mix) / 255.0f;
            blue            = (160.0f + (186.0f - 160.0f) * mix) / 255.0f;
            size            = 0.8f + ParticleInitRandom01(rng) * 0.6f;
            alpha           = std::sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f;
        } else if (zone < 0.69f) {
            ringRadius = radius * (1.95f + ParticleInitRandom01(rng) * 0.075f);
            ParticleInitUnpackColor(0x050505U, red, green, blue);
            size  = 0.3f;
            alpha = 0.1f;
        } else if (zone < 0.99f) {
            ringRadius = radius * (2.025f + ParticleInitRandom01(rng) * 0.245f);
            ParticleInitUnpackColor(0x989085U, red, green, blue);
            size  = 0.7f;
            alpha = ringRadius > radius * 2.2f && ringRadius < radius * 2.21f ? 0.1f : 0.6f;
        } else {
            ringRadius = radius * (2.32f + ParticleInitRandom01(rng) * 0.02f);
            ParticleInitUnpackColor(0xAFAFA0U, red, green, blue);
            alpha = 0.7f;
        }
        const float theta    = ParticleInitRandom01(rng) * 6.28318f;
        particle.position[0] = ringRadius * std::cos(theta);
        particle.position[1] = (ParticleInitRandom01(rng) - 0.5f) * (ringRadius > radius * 2.3f ? 0.4f : 0.15f);
        particle.position[2] = ringRadius * std::sin(theta);
        particle.position[3] = size;
        particle.speed       = 8.0f / std::sqrt(ringRadius);
        particle.isRing      = 1U;
    }
    particle.color = ParticleInitPackColor(red, green, blue, alpha);
    return particle;
}

} // namespace ParticleSaturn::ShaderAbi
