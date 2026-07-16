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

float Random01(thread uint& state) {
    state = state * 747796405U + 2891336453U;
    uint value = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    value = (value >> 22U) ^ value;
    return float(value) / 4294967295.0f;
}

uint PackColor(float3 color, float alpha) {
    const uint r = uint(clamp(color.r, 0.0f, 1.0f) * 255.0f);
    const uint g = uint(clamp(color.g, 0.0f, 1.0f) * 255.0f);
    const uint b = uint(clamp(color.b, 0.0f, 1.0f) * 255.0f);
    const uint a = uint(clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return r | (g << 8U) | (b << 16U) | (a << 24U);
}

kernel void InitializeParticles(device Particle* particles [[buffer(0)]],
                                constant uint& seed [[buffer(1)]],
                                uint id [[thread_position_in_grid]]) {
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
        constexpr float3 colors[4] = {float3(0.890f, 0.855f, 0.773f), float3(0.788f, 0.627f, 0.439f),
                                      float3(0.890f, 0.855f, 0.773f), float3(0.690f, 0.553f, 0.333f)};
        color = colors[(index % 4 + 4) % 4]; alpha = 0.8f;
        particles[id].position.w = 1.0f + Random01(rng) * 0.8f;
        particles[id].speed = 0.0f; particles[id].isRing = 0U;
    } else {
        const float z = Random01(rng);
        float ringRadius; float size; float opacity;
        if (z < 0.15f) { ringRadius = radius * (1.235f + Random01(rng) * 0.29f); color = float3(0.165f, 0.145f, 0.125f); size = 0.5f; opacity = 0.3f; }
        else if (z < 0.65f) { const float t = Random01(rng); ringRadius = radius * (1.525f + t * 0.425f); color = mix(float3(0.804f, 0.749f, 0.627f), float3(0.863f, 0.796f, 0.729f), t); size = 0.8f + Random01(rng) * 0.6f; opacity = sin(ringRadius * 2.0f) > 0.8f ? 1.02f : 0.85f; }
        else if (z < 0.69f) { ringRadius = radius * (1.95f + Random01(rng) * 0.075f); color = float3(0.02f); size = 0.3f; opacity = 0.1f; }
        else if (z < 0.99f) { ringRadius = radius * (2.025f + Random01(rng) * 0.245f); color = float3(0.596f, 0.565f, 0.522f); size = 0.7f; opacity = ringRadius > radius * 2.2f && ringRadius < radius * 2.21f ? 0.1f : 0.6f; }
        else { ringRadius = radius * (2.32f + Random01(rng) * 0.02f); color = float3(0.686f); size = 1.0f; opacity = 0.7f; }
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
    const uint value = Hash(id ^ seed);
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
                    uint2 id [[thread_position_in_grid]]) {
    if (id.x >= ldr.get_width() || id.y >= ldr.get_height()) return;
    const float2 uv = (float2(id) + 0.5f) / float2(ldr.get_width(), ldr.get_height());
    float3 color = hdr.read(id).rgb + SampleBilinear(bloom, uv) * 0.25f;
    color = (color * (2.51f * color + 0.03f)) / (color * (2.43f * color + 0.59f) + 0.14f);
    ldr.write(float4(clamp(color, 0.0f, 1.0f), 1.0f), id);
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

struct PanelMask { float left; float top; float width; float height; };

kernel void BuildAcrylicPanelMask(texture2d<float, access::write> output [[texture(0)]], constant PanelMask& panel [[buffer(0)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= output.get_width() || id.y >= output.get_height()) return;
    const bool inside = float(id.x) >= panel.left && float(id.x) < panel.left + panel.width &&
                        float(id.y) >= panel.top && float(id.y) < panel.top + panel.height;
    output.write(inside ? float4(0.078f, 0.078f, 0.098f, 1.0f) : float4(0.0f), id);
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
    constexpr uint thickness = 2U;
    constexpr uint width = 20U;
    constexpr uint height = 36U;
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
};

struct RenderConstants { float aspect; float screenHeight; float time; float scale; float rotationX; float rotationY; };

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

vertex PointVertex ParticleVertex(const device Particle* particles [[buffer(0)]], constant RenderConstants& constants [[buffer(1)]],
                                  uint id [[vertex_id]]) {
    const Particle particle = particles[id];
    const float3 position = RotateSaturn(particle.position.xyz * constants.scale, constants.rotationX, constants.rotationY);
    const float distance = 100.0f - position.z;
    const float focalLength = 1.0f / tan(1.047f * 0.5f);
    PointVertex output;
    output.position = float4(position.x * focalLength / (constants.aspect * distance), position.y * focalLength / distance, 0.0f, 1.0f);
    output.color = UnpackColor(particle.color);
    output.pointSize = clamp(particle.position.w * 350.0f * 0.55f / distance * (constants.screenHeight / 1080.0f), 0.0f, 300.0f);
    output.distance = distance;
    output.scale = constants.scale;
    output.isRing = float(particle.isRing);
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
    const float alpha = glow * input.color.a * (0.25f + 0.45f * smoothstep(0.0f, 0.5f, t)) * depthAlpha;
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
    return output;
}

fragment float4 StarFragment(PointVertex input [[stage_in]], float2 point [[point_coord]]) {
    const float radiusSquared = dot(point * 2.0f - 1.0f, point * 2.0f - 1.0f);
    if (radiusSquared > 1.0f) discard_fragment();
    const float noise = fract(sin(dot(input.position.xy, float2(12.9f, 78.2f))) * 43758.5f);
    const float twinkle = 0.7f + 0.3f * sin(input.scale * 2.0f + (noise + input.color.a) * 10.0f);
    return float4(input.color.rgb * twinkle * 3.0f, pow(1.0f - radiusSquared, 1.5f) * 0.9f);
}
