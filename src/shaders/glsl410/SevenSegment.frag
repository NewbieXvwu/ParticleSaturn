#version 410 core

layout(location = 0) out vec4 outColor;

uniform uint uFramesPerSecond;
uniform uvec2 uOutputSize;

bool isSevenSegmentPixel(uvec2 pixel, uint digit, uint digitIndex) {
    const uint segmentMasks[10] = uint[10](0x3fu, 0x06u, 0x5bu, 0x4fu, 0x66u,
                                           0x6du, 0x7du, 0x07u, 0x7fu, 0x6fu);
    uint originX = uOutputSize.x - 60u - digitIndex * 30u;
    const uint originY = 4u;
    if (pixel.x < originX || pixel.y < originY) return false;
    uint x = pixel.x - originX;
    uint y = pixel.y - originY;
    const uint width = 20u;
    const uint height = 36u;
    if (x >= width || y >= height) return false;
    uint mask = segmentMasks[digit];
    bool top = (mask & 0x01u) != 0u && y == 0u;
    bool upperRight = (mask & 0x02u) != 0u && x == width - 1u && y <= height / 2u;
    bool lowerRight = (mask & 0x04u) != 0u && x == width - 1u && y >= height / 2u;
    bool bottom = (mask & 0x08u) != 0u && y == height - 1u;
    bool lowerLeft = (mask & 0x10u) != 0u && x == 0u && y >= height / 2u;
    bool upperLeft = (mask & 0x20u) != 0u && x == 0u && y <= height / 2u;
    bool middle = (mask & 0x40u) != 0u && y == height / 2u;
    return top || upperRight || lowerRight || bottom || lowerLeft || upperLeft || middle;
}

void main() {
    uvec2 pixel = uvec2(uint(gl_FragCoord.x), uOutputSize.y - 1u - uint(gl_FragCoord.y));
    uint digitCount = uFramesPerSecond >= 100u ? 3u : (uFramesPerSecond >= 10u ? 2u : 1u);
    bool lit = false;
    for (uint digitIndex = 0u; digitIndex < digitCount; ++digitIndex) {
        uint divisor = digitIndex == 0u ? 1u : (digitIndex == 1u ? 10u : 100u);
        lit = lit || isSevenSegmentPixel(pixel, (uFramesPerSecond / divisor) % 10u, digitIndex);
    }
    if (!lit) discard;
    vec3 color = uFramesPerSecond > 50u ? vec3(0.3, 1.0, 0.3) :
                 (uFramesPerSecond > 30u ? vec3(1.0, 0.6, 0.0) : vec3(1.0, 0.2, 0.2));
    outColor = vec4(color, 1.0);
}
