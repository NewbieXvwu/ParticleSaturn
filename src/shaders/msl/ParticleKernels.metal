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

kernel void InitializeStars(device float4* stars [[buffer(0)]],
                            constant uint& seed [[buffer(1)]],
                            uint id [[thread_position_in_grid]]) {
    const uint value = Hash(id ^ seed);
    const float x = float(value & 0x3ffU) / 512.0f - 1.0f;
    const float y = float((value >> 10) & 0x3ffU) / 512.0f - 1.0f;
    const float z = float((value >> 20) & 0x3ffU) / 512.0f - 1.0f;
    stars[id] = float4(normalize(float3(x, y, z)) * 100.0f, 1.0f);
}

kernel void ToneMap(texture2d<float, access::read> hdr [[texture(0)]],
                    texture2d<float, access::write> ldr [[texture(1)]],
                    uint2 id [[thread_position_in_grid]]) {
    if (id.x >= ldr.get_width() || id.y >= ldr.get_height()) return;
    float3 color = hdr.read(id).rgb;
    color = (color * (2.51f * color + 0.03f)) / (color * (2.43f * color + 0.59f) + 0.14f);
    ldr.write(float4(clamp(color, 0.0f, 1.0f), 1.0f), id);
}

kernel void BloomDownsample(texture2d<float, access::read> source [[texture(0)]],
                            texture2d<float, access::write> target [[texture(1)]],
                            uint2 id [[thread_position_in_grid]]) {
    if (id.x >= target.get_width() || id.y >= target.get_height()) return;
    const uint2 base = id * 6U;
    float3 color = source.read(base).rgb;
    target.write(float4(max(color - 1.0f, 0.0f), 1.0f), id);
}

kernel void KawaseBlur(texture2d<float, access::read> source [[texture(0)]],
                       texture2d<float, access::write> target [[texture(1)]],
                       uint2 id [[thread_position_in_grid]]) {
    if (id.x >= target.get_width() || id.y >= target.get_height()) return;
    const int2 p = int2(id);
    const int2 size = int2(source.get_width() - 1, source.get_height() - 1);
    float4 color = source.read(uint2(clamp(p + int2(-1, -1), int2(0), size))) +
                   source.read(uint2(clamp(p + int2(1, -1), int2(0), size))) +
                   source.read(uint2(clamp(p + int2(-1, 1), int2(0), size))) +
                   source.read(uint2(clamp(p + int2(1, 1), int2(0), size)));
    target.write(color * 0.25f, id);
}

struct AcrylicConstants {
    float blurRadius;
    float opacity;
};

kernel void UiKawaseBlur(texture2d<float, access::read> source [[texture(0)]],
                         texture2d<float, access::write> target [[texture(1)]],
                         constant AcrylicConstants& constants [[buffer(0)]],
                         uint2 id [[thread_position_in_grid]]) {
    if (id.x >= target.get_width() || id.y >= target.get_height()) return;
    const int offset = max(1, int(round(constants.blurRadius)));
    const int2 p = int2(id);
    const int2 size = int2(source.get_width() - 1, source.get_height() - 1);
    const int2 lower = int2(0);
    const float4 color = source.read(uint2(clamp(p + int2(-offset, -offset), lower, size))) +
                         source.read(uint2(clamp(p + int2( offset, -offset), lower, size))) +
                         source.read(uint2(clamp(p + int2(-offset,  offset), lower, size))) +
                         source.read(uint2(clamp(p + int2( offset,  offset), lower, size)));
    target.write(color * 0.25f, id);
}

kernel void AcrylicComposite(texture2d<float, access::read> scene [[texture(0)]],
                             texture2d<float, access::read> blurredScene [[texture(1)]],
                             texture2d<float, access::read> overlay [[texture(2)]],
                             texture2d<float, access::write> output [[texture(3)]],
                             constant AcrylicConstants& constants [[buffer(0)]],
                             uint2 id [[thread_position_in_grid]]) {
    if (id.x >= output.get_width() || id.y >= output.get_height()) return;
    const float4 sceneColor = scene.read(id);
    const float4 overlayColor = overlay.read(id);
    const float mask = clamp(overlayColor.a * constants.opacity, 0.0f, 1.0f);
    const float3 acrylic = mix(blurredScene.read(id).rgb, float3(0.12f, 0.15f, 0.18f), 0.18f);
    const float3 background = mix(sceneColor.rgb, acrylic, mask);
    output.write(float4(mix(background, overlayColor.rgb, overlayColor.a), 1.0f), id);
}

bool IsSevenSegmentPixel(uint2 pixel, uint digit, uint digitIndex, uint scale) {
    constexpr uint SegmentMasks[10] = {0x3fU, 0x06U, 0x5bU, 0x4fU, 0x66U, 0x6dU, 0x7dU, 0x07U, 0x7fU, 0x6fU};
    const uint x = pixel.x - (8U + digitIndex * 10U * scale);
    const uint y = pixel.y - 8U;
    const uint thickness = scale;
    const uint width = 7U * scale;
    const uint height = 14U * scale;
    if (x >= width || y >= height) return false;
    const uint mask = SegmentMasks[digit];
    const bool top = (mask & 0x01U) != 0U && y < thickness && x >= thickness && x + thickness < width;
    const bool upperRight = (mask & 0x02U) != 0U && x + thickness >= width && y >= thickness && y + thickness < height / 2U;
    const bool lowerRight = (mask & 0x04U) != 0U && x + thickness >= width && y > height / 2U && y + thickness < height;
    const bool bottom = (mask & 0x08U) != 0U && y + thickness >= height && x >= thickness && x + thickness < width;
    const bool lowerLeft = (mask & 0x10U) != 0U && x < thickness && y > height / 2U && y + thickness < height;
    const bool upperLeft = (mask & 0x20U) != 0U && x < thickness && y >= thickness && y + thickness < height / 2U;
    const bool middle = (mask & 0x40U) != 0U && y >= height / 2U - thickness / 2U && y <= height / 2U + thickness / 2U &&
                        x >= thickness && x + thickness < width;
    return top || upperRight || lowerRight || bottom || lowerLeft || upperLeft || middle;
}

kernel void RenderSevenSegmentFps(texture2d<float, access::write> output [[texture(0)]],
                                  constant uint& framesPerSecond [[buffer(0)]],
                                  uint2 id [[thread_position_in_grid]]) {
    if (id.x >= output.get_width() || id.y >= output.get_height()) return;
    const uint scale = max(1U, output.get_height() / 180U);
    for (uint digitIndex = 0U; digitIndex < 3U; ++digitIndex) {
        const uint divisor = digitIndex == 0U ? 100U : (digitIndex == 1U ? 10U : 1U);
        if (IsSevenSegmentPixel(id, (framesPerSecond / divisor) % 10U, digitIndex, scale)) {
            output.write(float4(0.85f, 0.97f, 1.0f, 1.0f), id);
            return;
        }
    }
}
