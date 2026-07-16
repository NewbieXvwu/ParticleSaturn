#include <metal_stdlib>

using namespace metal;

struct Particle {
    float4 position;
    uint color;
    float speed;
    uint isRing;
    uint padding;
};

struct SimulationConstants {
    float deltaTime;
    float handScale;
    float handTracked;
    uint particleCount;
};

uint Hash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

kernel void InitializeParticles(device Particle* particles [[buffer(0)]],
                                constant uint& seed [[buffer(1)]],
                                uint id [[thread_position_in_grid]]) {
    const uint value = Hash(id ^ seed);
    const float angle = float(value & 0xffffU) * 0.000095875f;
    const float radius = 1.2f + float((value >> 16) & 0xffU) * 0.018f;
    const bool ring = (value & 3U) != 0U;
    particles[id].position = float4(cos(angle) * radius, ring ? 0.0f : 0.25f, sin(angle) * radius, 1.0f);
    particles[id].color = 0xffffffffU;
    particles[id].speed = ring ? 0.2f + float(value & 0x7fU) * 0.002f : 0.03f;
    particles[id].isRing = ring ? 1U : 0U;
    particles[id].padding = 0U;
}

kernel void SimulateParticles(const device Particle* input [[buffer(0)]],
                              device Particle* output [[buffer(1)]],
                              constant SimulationConstants& constants [[buffer(2)]],
                              uint id [[thread_position_in_grid]]) {
    if (id >= constants.particleCount) return;
    Particle particle = input[id];
    const float scale = mix(1.0f, constants.handScale, constants.handTracked);
    const float angle = (particle.isRing == 0U ? 0.03f : particle.speed * 0.2f) * constants.deltaTime * scale;
    const float c = cos(angle);
    const float s = sin(angle);
    particle.position.xz = float2(particle.position.x * c - particle.position.z * s,
                                  particle.position.x * s + particle.position.z * c);
    output[id] = particle;
}
