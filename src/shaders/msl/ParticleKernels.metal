#include <metal_stdlib>
#include "Particle.metal"

using namespace metal;

struct SimulationConstants {
    float deltaTime;
    float handScale;
    float handTracked;
    uint particleCount;
};

uint PcgHash(uint input) {
    const uint state = input * 747796405U + 2891336453U;
    const uint word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

float Random01(thread uint& state) {
    state = PcgHash(state);
    return float(state) * (1.0f / 4294967296.0f);
}

uint PackColor(float3 color, float alpha) {
    const uint r = uint(clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint g = uint(clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint b = uint(clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint a = uint(clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return r | (g << 8U) | (b << 16U) | (a << 24U);
}

float3 HexToRgb(uint color) {
    return float3(float((color >> 16U) & 0xffU), float((color >> 8U) & 0xffU), float(color & 0xffU)) / 255.0f;
}

kernel void InitializeParticles(device Particle* particles [[buffer(0)]],
                                constant uint& seed [[buffer(1)]],
                                constant uint& particleCapacity [[buffer(2)]],
                                uint id [[thread_position_in_grid]]) {
    // 线程组调度可能补齐到执行宽度的整数倍，越界线程直接返回。
    if (id >= particleCapacity) return;
    constexpr float radius = 18.0f;
    uint rng = id * 1973U + seed * 9277U + 26699U;
    const float type = Random01(rng);
    float3 color = float3(1.0f);
    float alpha = 1.0f;
    if (type < 0.25f) {
        const float theta = 6.28318f * Random01(rng);
        const float phi = acos(2.0f * Random01(rng) - 1.0f);
        particles[id].position.xyz = float3(radius * sin(phi) * cos(theta), radius * cos(phi) * 0.9f,
                                             radius * sin(phi) * sin(theta));
        const float latitude = (particles[id].position.y / (0.9f * radius) + 1.0f) * 0.5f;
        const int index = int(latitude * 4.0f + cos(latitude * 40.0f) * 0.8f + cos(latitude * 15.0f) * 0.4f);
        constexpr uint colors[4] = {0xE3DAC5U, 0xC9A070U, 0xE3DAC5U, 0xB08D55U};
        color = HexToRgb(colors[(index % 4 + 4) % 4]); alpha = 0.8f;
        particles[id].position.w = 1.0f + Random01(rng) * 0.8f;
        particles[id].speed = 0.0f; particles[id].isRing = 0U;
    } else {
        const float z = Random01(rng);
        float ringRadius; float size; float opacity;
        if (z < 0.15f) { ringRadius = radius * (1.235f + Random01(rng) * 0.29f); color = HexToRgb(0x2A2520U); size = 0.5f; opacity = 0.3f; }
        else if (z < 0.65f) { const float t = Random01(rng); ringRadius = radius * (1.525f + t * 0.425f); color = mix(HexToRgb(0xCDBFA0U), HexToRgb(0xDCCBBAU), t); size = 0.8f + Random01(rng) * 0.6f; opacity = sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f; }
        else if (z < 0.69f) { ringRadius = radius * (1.95f + Random01(rng) * 0.075f); color = HexToRgb(0x050505U); size = 0.3f; opacity = 0.1f; }
        else if (z < 0.99f) { ringRadius = radius * (2.025f + Random01(rng) * 0.245f); color = HexToRgb(0x989085U); size = 0.7f; opacity = ringRadius > radius * 2.2f && ringRadius < radius * 2.21f ? 0.1f : 0.6f; }
        else { ringRadius = radius * (2.32f + Random01(rng) * 0.02f); color = HexToRgb(0xAFAFA0U); size = 1.0f; opacity = 0.7f; }
        const float theta = Random01(rng) * 6.28318f;
        particles[id].position = float4(ringRadius * cos(theta), (Random01(rng) - 0.5f) * (ringRadius > radius * 2.3f ? 0.4f : 0.15f), ringRadius * sin(theta), size);
        alpha = opacity; particles[id].speed = 8.0f / sqrt(ringRadius); particles[id].isRing = 1U;
    }
    particles[id].color = PackColor(color, alpha);
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
    const uint value = PcgHash(id ^ seed);
    const float x = float(value & 0x3ffU) / 512.0f - 1.0f;
    const float y = float((value >> 10) & 0x3ffU) / 512.0f - 1.0f;
    const float z = float((value >> 20) & 0x3ffU) / 512.0f - 1.0f;
    stars[id] = float4(normalize(float3(x, y, z)) * 100.0f, 1.0f);
}

float3 SampleBilinear(texture2d<float, access::read> texture, float2 uv) {
    const int2 size = int2(texture.get_width(), texture.get_height());
    const float2 coordinate = uv * float2(size) - 0.5f;
    const int2 base = int2(floor(coordinate));
    const float2 fraction = fract(coordinate);
    const int2 maximum = size - 1;
    const float3 c00 = texture.read(uint2(clamp(base, int2(0), maximum))).rgb;
    const float3 c10 = texture.read(uint2(clamp(base + int2(1, 0), int2(0), maximum))).rgb;
    const float3 c01 = texture.read(uint2(clamp(base + int2(0, 1), int2(0), maximum))).rgb;
    const float3 c11 = texture.read(uint2(clamp(base + int2(1, 1), int2(0), maximum))).rgb;
    return mix(mix(c00, c10, fraction.x), mix(c01, c11, fraction.x), fraction.y);
}

kernel void ToneMapWithBloom(texture2d<float, access::read> hdr [[texture(0)]],
                    texture2d<float, access::read> bloom [[texture(1)]],
                    texture2d<float, access::write> ldr [[texture(2)]],
                    constant float& bloomStrength [[buffer(0)]],
                    constant float& transparent [[buffer(1)]],
                    uint2 id [[thread_position_in_grid]]) {
    if (id.x >= ldr.get_width() || id.y >= ldr.get_height()) return;
    const float2 uv = (float2(id) + 0.5f) / float2(ldr.get_width(), ldr.get_height());
    float3 color = hdr.read(id).rgb + SampleBilinear(bloom, uv) * bloomStrength;
    // Keep the Diligent fullscreen composition curve.  The old implementation
    // only compresses HDR highlights and leaves ordinary scene color linear.
    const float maximum = max(color.r, max(color.g, color.b));
    const float highlightWeight = maximum >= 1.0f ? 0.5f : 0.0f;
    const float3 compressed = color / (color + float3(1.0f));
    color = mix(color, compressed, highlightWeight);
    const float alpha = clamp(mix(1.0f, maximum, transparent), 0.0f, 1.0f);
    ldr.write(float4(color * alpha, alpha), id);
}

struct BloomConstants { float texelX; float texelY; float offset; float threshold; };

float3 BrightPass(float3 color, float threshold) {
    const float maximum = max(color.r, max(color.g, color.b));
    if (threshold <= 0.0001f) return color;
    return color * smoothstep(threshold, threshold * 2.0f, maximum);
}

kernel void BloomDownsample(texture2d<float, access::read> source [[texture(0)]],
                            texture2d<float, access::write> target [[texture(1)]],
                            constant BloomConstants& constants [[buffer(0)]],
                            uint2 id [[thread_position_in_grid]]) {
    if (id.x >= target.get_width() || id.y >= target.get_height()) return;
    const float2 uv = (float2(id) + 0.5f) / float2(target.get_width(), target.get_height());
    const float2 halfPixel = float2(constants.texelX, constants.texelY) * 0.5f;
    const float3 color = (SampleBilinear(source, uv + float2(-halfPixel.x, -halfPixel.y)) +
                          SampleBilinear(source, uv + float2( halfPixel.x, -halfPixel.y)) +
                          SampleBilinear(source, uv + float2(-halfPixel.x,  halfPixel.y)) +
                          SampleBilinear(source, uv + float2( halfPixel.x,  halfPixel.y))) * 0.25f;
    target.write(float4(BrightPass(color, constants.threshold), 1.0f), id);
}

kernel void KawaseBlur(texture2d<float, access::read> source [[texture(0)]],
                       texture2d<float, access::write> target [[texture(1)]],
                       constant BloomConstants& constants [[buffer(0)]],
                       uint2 id [[thread_position_in_grid]]) {
    if (id.x >= target.get_width() || id.y >= target.get_height()) return;
    const float2 uv = (float2(id) + 0.5f) / float2(target.get_width(), target.get_height());
    const float2 offset = float2(constants.texelX, constants.texelY) * (constants.offset + 0.5f);
    const float3 color = (SampleBilinear(source, uv + float2(-offset.x, -offset.y)) +
                          SampleBilinear(source, uv + float2( offset.x, -offset.y)) +
                          SampleBilinear(source, uv + float2(-offset.x,  offset.y)) +
                          SampleBilinear(source, uv + float2( offset.x,  offset.y))) * 0.25f;
    target.write(float4(color, 1.0f), id);
}

struct AcrylicConstants {
    float tintR;
    float tintG;
    float tintB;
    float baseOpacity;
    float saturation;
    float adaptive;
    float darkMode;
    float exclusion;
};

kernel void AcrylicComposite(texture2d<float, access::read> blurredScene [[texture(0)]],
                             texture2d<float, access::write> output [[texture(1)]],
                             constant AcrylicConstants& constants [[buffer(0)]],
                             uint2 id [[thread_position_in_grid]]) {
    if (id.x >= output.get_width() || id.y >= output.get_height()) return;
    float3 color = blurredScene.read(id).rgb;
    const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    color = clamp(float3(luminance) + (color - float3(luminance)) * constants.saturation, 0.0f, 1.0f);
    const float adjustedLuminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    float opacity = constants.baseOpacity;
    opacity = constants.darkMode > 0.5f
        ? clamp(opacity + (adjustedLuminance - 0.5f) * constants.adaptive, 0.0f, 1.0f)
        : clamp(opacity + (0.5f - adjustedLuminance) * constants.adaptive, 0.0f, 1.0f);
    const float3 tint = float3(constants.tintR, constants.tintG, constants.tintB);
    const float3 exclusion = (color + tint) - (2.0f * color * tint);
    const float3 mixed = mix(color, exclusion, clamp(constants.exclusion, 0.0f, 1.0f));
    output.write(float4(mix(color, mixed, opacity), 1.0f), id);
}

bool IsSevenSegmentPixel(uint2 pixel, uint digit, uint digitIndex, uint viewportWidth) {
    constexpr uint SegmentMasks[10] = {0x3fU, 0x06U, 0x5bU, 0x4fU, 0x66U, 0x6dU, 0x7dU, 0x07U, 0x7fU, 0x6fU};
    // Mirrors DiligentBackend::RenderSevenSegmentFPS: size=20, spacing=30,
    // rightmost digit starts at viewportWidth - 60 and the baseline is 40 px from the top.
    const uint originX = viewportWidth - 60U - digitIndex * 30U;
    const uint originY = 4U;
    if (pixel.x < originX || pixel.y < originY) return false;
    const uint x = pixel.x - originX;
    const uint y = pixel.y - originY;
    constexpr uint width = 20U;
    constexpr uint height = 36U;
    if (x >= width || y >= height) return false;
    const uint mask = SegmentMasks[digit];
    // Diligent renders a 20 by 36 line list.  Keep individual one-pixel
    // segments instead of turning those lines into wider filled rectangles.
    const bool top = (mask & 0x01U) != 0U && y == 0U;
    const bool upperRight = (mask & 0x02U) != 0U && x == width - 1U && y <= height / 2U;
    const bool lowerRight = (mask & 0x04U) != 0U && x == width - 1U && y >= height / 2U;
    const bool bottom = (mask & 0x08U) != 0U && y == height - 1U;
    const bool lowerLeft = (mask & 0x10U) != 0U && x == 0U && y >= height / 2U;
    const bool upperLeft = (mask & 0x20U) != 0U && x == 0U && y <= height / 2U;
    const bool middle = (mask & 0x40U) != 0U && y == height / 2U;
    return top || upperRight || lowerRight || bottom || lowerLeft || upperLeft || middle;
}

kernel void RenderSevenSegmentFps(texture2d<float, access::write> output [[texture(0)]],
                                  constant uint& framesPerSecond [[buffer(0)]],
                                  uint2 id [[thread_position_in_grid]]) {
    if (id.x >= output.get_width() || id.y >= output.get_height()) return;
    const uint digitCount = framesPerSecond >= 100U ? 3U : (framesPerSecond >= 10U ? 2U : 1U);
    const float3 color = framesPerSecond > 50U ? float3(0.3f, 1.0f, 0.3f) :
                         (framesPerSecond > 30U ? float3(1.0f, 0.6f, 0.0f) : float3(1.0f, 0.2f, 0.2f));
    for (uint digitIndex = 0U; digitIndex < digitCount; ++digitIndex) {
        const uint divisor = digitIndex == 0U ? 1U : (digitIndex == 1U ? 10U : 100U);
        if (IsSevenSegmentPixel(id, (framesPerSecond / divisor) % 10U, digitIndex, output.get_width())) {
            output.write(float4(color, 1.0f), id);
            return;
        }
    }
}

struct PointVertex {
    float4 position [[position]];
    float4 color;
    float pointSize [[point_size]];
    float distance;
    float scale;
    float isRing;
    float densityCompensation;
};

struct RenderConstants {
    float aspect;
    float screenHeight;
    float time;
    float scale;
    float rotationX;
    float rotationY;
    float pixelRatio;
    float densityCompensation;
    uint particleCount;
};

float4 UnpackColor(uint color) {
    return float4(float(color & 255U), float((color >> 8U) & 255U), float((color >> 16U) & 255U), float((color >> 24U) & 255U)) / 255.0f;
}

float3 RotateSaturn(float3 position, float rotationX, float rotationY) {
    const float cz = cos(0.466f); const float sz = sin(0.466f);
    const float cy = cos(rotationY); const float sy = sin(rotationY);
    const float cx = cos(rotationX); const float sx = sin(rotationX);
    const float3 zRotated = float3(position.x * cz - position.y * sz, position.x * sz + position.y * cz, position.z);
    const float3 yRotated = float3(zRotated.x * cy + zRotated.z * sy, zRotated.y,
                                   -zRotated.x * sy + zRotated.z * cy);
    return float3(yRotated.x, yRotated.y * cx - yRotated.z * sx, yRotated.y * sx + yRotated.z * cx);
}

float ParticleHash(float value) {
    uint bits = as_type<uint>(value);
    bits = ((bits >> 16U) ^ bits) * 0x45d9f3bU;
    bits = ((bits >> 16U) ^ bits) * 0x45d9f3bU;
    bits = (bits >> 16U) ^ bits;
    return float(bits) * (1.0f / 4294967296.0f);
}

float FastSin(float value) {
    constexpr float twoPi = 6.28318530718f;
    constexpr float pi = 3.14159265359f;
    value = fmod(value, twoPi);
    value = value > pi ? value - twoPi : value;
    const float squared = value * value;
    return value * (1.0f - squared * (0.16666667f - squared * (0.00833333f - squared * 0.0001984f)));
}

vertex PointVertex ParticleVertex(const device Particle* particles [[buffer(0)]], constant RenderConstants& constants [[buffer(1)]],
                                  uint id [[vertex_id]]) {
    const Particle particle = particles[id];
    const float3 position = RotateSaturn(particle.position.xyz * constants.scale, constants.rotationX, constants.rotationY);
    const float distance = 100.0f - position.z;
    const float focalLength = 1.0f / tan(1.047f * 0.5f);
    float3 viewPosition = float3(position.x, position.y, position.z - 100.0f);
    float chaos = smoothstep(25.0f, 0.1f, distance);
    chaos = chaos * chaos * chaos;
    if (chaos > 0.001f) {
        const float highFrequencyTime = constants.time * 40.0f;
        const float3 scaledPosition = particle.position.xyz * 10.0f;
        const float3 noise = float3(FastSin(highFrequencyTime + scaledPosition.x) * ParticleHash(particle.position.y * 43758.5f) * 0.5f,
                                    FastSin(highFrequencyTime + scaledPosition.y + 1.5708f) * ParticleHash(particle.position.x * 43758.5f) * 0.5f,
                                    FastSin(highFrequencyTime * 0.5f) * ParticleHash(particle.position.z * 43758.5f) * 0.5f) * 3.0f;
        viewPosition += noise * chaos;
    }
    const float projectedDistance = max(-viewPosition.z, 0.001f);
    PointVertex output;
    output.position = float4(viewPosition.x * focalLength / (constants.aspect * projectedDistance),
                             viewPosition.y * focalLength / projectedDistance, 0.0f, 1.0f);
    output.color = UnpackColor(particle.color);
    const float nearMask = distance <= 50.0f ? 1.0f : 0.0f;
    const float ringFactor = mix(mix(1.0f, 0.8f, nearMask), 1.0f, float(particle.isRing));
    const float pointSize = particle.position.w * 350.0f * 0.55f / max(distance, 0.1f) *
                            (constants.screenHeight / 1080.0f) * ringFactor * pow(max(constants.pixelRatio, 0.0001f), 0.8f);
    output.pointSize = clamp(pointSize, 0.0f, 300.0f * (constants.screenHeight / 1080.0f));
    output.distance = distance;
    output.scale = constants.scale;
    output.isRing = float(particle.isRing);
    output.densityCompensation = constants.densityCompensation;
    return output;
}

fragment float4 ParticleFragment(PointVertex input [[stage_in]], float2 point [[point_coord]]) {
    const float radiusSquared = dot(point * 2.0f - 1.0f, point * 2.0f - 1.0f);
    if (radiusSquared > 1.0f) discard_fragment();
    const float glow = smoothstep(1.0f, 0.4f, radiusSquared);
    const float t = clamp((input.scale - 0.15f) * 0.4255f, 0.0f, 1.0f);
    const float smoothedT = smoothstep(0.1f, 0.9f, t);
    float3 color = mix(float3(0.35f, 0.22f, 0.05f), input.color.rgb, smoothedT) * (0.2f + t);
    const float closeMix = smoothstep(40.0f, 0.0f, input.distance);
    const float3 ringColor = color + float3(0.15f, 0.12f, 0.1f) * closeMix;
    const float3 bodyColor = mix(color, pow(input.color.rgb, float3(1.4f)) * 1.5f, closeMix * 0.8f);
    color = mix(bodyColor, ringColor, input.isRing);
    const float depthAlpha = smoothstep(0.0f, 10.0f, input.distance);
    const float alpha = glow * input.color.a * (0.25f + 0.45f * smoothstep(0.0f, 0.5f, t)) * depthAlpha * input.densityCompensation;
    return float4(color, alpha);
}

// ============================================================================
// Object Shader Path (Metal 3+)
// ============================================================================

struct QuadVertex {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float distance;
    float scale;
    float isRing;
    float densityCompensation;
};

using ParticleMesh = metal::mesh<QuadVertex, void, 4, 2, metal::topology::triangle>;

struct ObjectPayload {
    uint particleId;
};

[[object, max_total_threads_per_threadgroup(32), max_total_threadgroups_per_mesh_grid(1)]]
void ParticleObjectShader(
    mesh_grid_properties meshGridProperties,
    const device Particle* particles [[buffer(0)]],
    constant RenderConstants& constants [[buffer(1)]],
    object_data ObjectPayload& payload [[payload]],
    uint threadId [[thread_position_in_threadgroup]],
    uint threadgroupId [[threadgroup_position_in_grid]]
) {
    const uint particleId = threadgroupId * 32 + threadId;
    payload.particleId = particleId;

    if (particleId < constants.particleCount) {
        meshGridProperties.set_threadgroups_per_grid(uint3(1, 1, 1));
    } else {
        meshGridProperties.set_threadgroups_per_grid(uint3(0, 0, 0));
    }
}

[[mesh, max_total_threads_per_threadgroup(1)]]
void ParticleMeshShader(
    ParticleMesh output,
    const device Particle* particles [[buffer(0)]],
    constant RenderConstants& constants [[buffer(1)]],
    const object_data ObjectPayload& payload [[payload]]
) {
    const uint particleId = payload.particleId;
    if (particleId >= constants.particleCount) {
        output.set_primitive_count(0);
        return;
    }

    const Particle particle = particles[particleId];
    const float3 position = RotateSaturn(particle.position.xyz * constants.scale, constants.rotationX, constants.rotationY);
    const float distance = 100.0f - position.z;
    const float focalLength = 1.0f / tan(1.047f * 0.5f);
    float3 viewPosition = float3(position.x, position.y, position.z - 100.0f);

    float chaos = smoothstep(25.0f, 0.1f, distance);
    chaos = chaos * chaos * chaos;
    if (chaos > 0.001f) {
        const float highFrequencyTime = constants.time * 40.0f;
        const float3 scaledPosition = particle.position.xyz * 10.0f;
        const float3 noise = float3(
            FastSin(highFrequencyTime + scaledPosition.x) * ParticleHash(particle.position.y * 43758.5f) * 0.5f,
            FastSin(highFrequencyTime + scaledPosition.y + 1.5708f) * ParticleHash(particle.position.x * 43758.5f) * 0.5f,
            FastSin(highFrequencyTime * 0.5f) * ParticleHash(particle.position.z * 43758.5f) * 0.5f
        ) * 3.0f;
        viewPosition += noise * chaos;
    }

    const float projectedDistance = max(-viewPosition.z, 0.001f);
    const float2 centerNDC = float2(
        viewPosition.x * focalLength / (constants.aspect * projectedDistance),
        viewPosition.y * focalLength / projectedDistance
    );

    const float4 color = UnpackColor(particle.color);
    const float nearMask = distance <= 50.0f ? 1.0f : 0.0f;
    const float ringFactor = mix(mix(1.0f, 0.8f, nearMask), 1.0f, float(particle.isRing));
    const float pointSize = particle.position.w * 350.0f * 0.55f / max(distance, 0.1f) *
                            (constants.screenHeight / 1080.0f) * ringFactor *
                            pow(max(constants.pixelRatio, 0.0001f), 0.8f);
    const float clampedSize = clamp(pointSize, 0.0f, 300.0f * (constants.screenHeight / 1080.0f));

    const float halfSizeNDC = clampedSize / constants.screenHeight;

    QuadVertex v0, v1, v2, v3;
    v0.position = float4(centerNDC.x - halfSizeNDC * constants.aspect, centerNDC.y - halfSizeNDC, 0.0f, 1.0f);
    v1.position = float4(centerNDC.x + halfSizeNDC * constants.aspect, centerNDC.y - halfSizeNDC, 0.0f, 1.0f);
    v2.position = float4(centerNDC.x - halfSizeNDC * constants.aspect, centerNDC.y + halfSizeNDC, 0.0f, 1.0f);
    v3.position = float4(centerNDC.x + halfSizeNDC * constants.aspect, centerNDC.y + halfSizeNDC, 0.0f, 1.0f);

    v0.color = v1.color = v2.color = v3.color = color;
    v0.distance = v1.distance = v2.distance = v3.distance = distance;
    v0.scale = v1.scale = v2.scale = v3.scale = constants.scale;
    v0.isRing = v1.isRing = v2.isRing = v3.isRing = float(particle.isRing);
    v0.densityCompensation = v1.densityCompensation = v2.densityCompensation = v3.densityCompensation = constants.densityCompensation;

    v0.uv = float2(0.0f, 0.0f);
    v1.uv = float2(1.0f, 0.0f);
    v2.uv = float2(0.0f, 1.0f);
    v3.uv = float2(1.0f, 1.0f);

    output.set_vertex(0, v0);
    output.set_vertex(1, v1);
    output.set_vertex(2, v2);
    output.set_vertex(3, v3);

    output.set_index(0, 0);
    output.set_index(1, 1);
    output.set_index(2, 2);
    output.set_index(3, 1);
    output.set_index(4, 3);
    output.set_index(5, 2);

    output.set_primitive_count(2);
}

fragment float4 ParticleQuadFragment(QuadVertex input [[stage_in]]) {
    const float2 point = input.uv;
    const float radiusSquared = dot(point * 2.0f - 1.0f, point * 2.0f - 1.0f);
    if (radiusSquared > 1.0f) discard_fragment();
    const float glow = smoothstep(1.0f, 0.4f, radiusSquared);
    const float t = clamp((input.scale - 0.15f) * 0.4255f, 0.0f, 1.0f);
    const float smoothedT = smoothstep(0.1f, 0.9f, t);
    float3 color = mix(float3(0.35f, 0.22f, 0.05f), input.color.rgb, smoothedT) * (0.2f + t);
    const float closeMix = smoothstep(40.0f, 0.0f, input.distance);
    const float3 ringColor = color + float3(0.15f, 0.12f, 0.1f) * closeMix;
    const float3 bodyColor = mix(color, pow(input.color.rgb, float3(1.4f)) * 1.5f, closeMix * 0.8f);
    color = mix(bodyColor, ringColor, input.isRing);
    const float depthAlpha = smoothstep(0.0f, 10.0f, input.distance);
    const float alpha = glow * input.color.a * (0.25f + 0.45f * smoothstep(0.0f, 0.5f, t)) * depthAlpha * input.densityCompensation;
    return float4(color, alpha);
}

struct Star { packed_float3 position; packed_float3 color; float size; float randomSeed; };

vertex PointVertex StarVertex(const device Star* stars [[buffer(0)]], constant RenderConstants& constants [[buffer(1)]],
                              uint id [[vertex_id]]) {
    PointVertex output;
    const Star star = stars[id];
    const float starRotation = constants.time * 0.005f;
    const float3 position = float3(star.position.x * cos(starRotation) + star.position.z * sin(starRotation),
                                   star.position.y,
                                   -star.position.x * sin(starRotation) + star.position.z * cos(starRotation));
    const float distance = 100.0f - position.z;
    const float focalLength = 1.0f / tan(1.047f * 0.5f);
    output.position = float4(position.x * focalLength / (constants.aspect * distance), position.y * focalLength / distance, 0.0f, 1.0f);
    output.color = float4(float3(star.color), star.randomSeed);
    output.pointSize = clamp(star.size * 1000.0f / distance, 1.0f, 8.0f);
    output.distance = distance; output.scale = constants.time; output.isRing = 0.0f;
    output.densityCompensation = 1.0f;
    return output;
}

fragment float4 StarFragment(PointVertex input [[stage_in]], float2 point [[point_coord]]) {
    const float radiusSquared = dot(point * 2.0f - 1.0f, point * 2.0f - 1.0f);
    if (radiusSquared > 1.0f) discard_fragment();
    const float noise = fract(sin(dot(input.position.xy, float2(12.9f, 78.2f))) * 43758.5f);
    const float twinkle = 0.7f + 0.3f * sin(input.scale * 2.0f + (noise + input.color.a) * 10.0f);
    return float4(input.color.rgb * twinkle * 3.0f, pow(1.0f - radiusSquared, 1.5f) * 0.9f);
}
