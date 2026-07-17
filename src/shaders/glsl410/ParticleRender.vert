#version 410 core

layout(location = 0) in vec4 inPosition;
layout(location = 1) in uint inColor;
layout(location = 2) in float inSpeed;
layout(location = 3) in uint inIsRing;

uniform float uTime;
uniform float uScale;
uniform float uRotationX;
uniform float uRotationY;
uniform float uAspect;
uniform float uScreenHeight;
uniform float uPixelRatio;
uniform float uDensityCompensation;
uniform float uAnalyticPhase;

out vec4 vColor;
out float vDistance;
out float vScale;
out float vIsRing;
out float vDensityCompensation;
out vec2 vPointCoord;

float hash(float value) {
    uint bits = floatBitsToUint(value);
    bits = ((bits >> 16u) ^ bits) * 0x45d9f3bu;
    bits = ((bits >> 16u) ^ bits) * 0x45d9f3bu;
    bits = (bits >> 16u) ^ bits;
    return float(bits) * (1.0 / 4294967296.0);
}

float fastSin(float value) {
    value = mod(value, 6.28318530718);
    value = value > 3.14159265359 ? value - 6.28318530718 : value;
    float squared = value * value;
    return value * (1.0 - squared * (0.16666667 - squared * (0.00833333 - squared * 0.0001984)));
}

vec3 rotateSaturn(vec3 position) {
    float cz = cos(0.466);
    float sz = sin(0.466);
    float cy = cos(uRotationY);
    float sy = sin(uRotationY);
    float cx = cos(uRotationX);
    float sx = sin(uRotationX);
    vec3 zRotated = vec3(position.x * cz - position.y * sz, position.x * sz + position.y * cz, position.z);
    vec3 yRotated = vec3(zRotated.x * cy + zRotated.z * sy, zRotated.y, -zRotated.x * sy + zRotated.z * cy);
    return vec3(yRotated.x, yRotated.y * cx - yRotated.z * sx, yRotated.y * sx + yRotated.z * cx);
}

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    vec2 corner = corners[gl_VertexID % 6];
    float orbitAngle = (inIsRing == 0u ? 0.03 : 0.2 * inSpeed) * uAnalyticPhase;
    float orbitC = cos(orbitAngle);
    float orbitS = sin(orbitAngle);
    vec3 orbitalPosition = vec3(inPosition.x * orbitC - inPosition.z * orbitS, inPosition.y,
                                inPosition.x * orbitS + inPosition.z * orbitC);
    vec3 position = rotateSaturn(orbitalPosition * uScale);
    float distance = 100.0 - position.z;
    vec3 viewPosition = vec3(position.xy, position.z - 100.0);
    float chaos = smoothstep(25.0, 0.1, distance);
    chaos *= chaos * chaos;
    if (chaos > 0.001) {
        float highFrequencyTime = uTime * 40.0;
        vec3 scaledPosition = inPosition.xyz * 10.0;
        vec3 noise = vec3(fastSin(highFrequencyTime + scaledPosition.x) * hash(inPosition.y * 43758.5) * 0.5,
                          fastSin(highFrequencyTime + scaledPosition.y + 1.5708) * hash(inPosition.x * 43758.5) * 0.5,
                          fastSin(highFrequencyTime * 0.5) * hash(inPosition.z * 43758.5) * 0.5) * 3.0;
        viewPosition += noise * chaos;
    }
    float projectedDistance = max(-viewPosition.z, 0.001);
    float focalLength = 1.0 / tan(1.047 * 0.5);
    vec2 ndcCenter = vec2(viewPosition.x * focalLength / (uAspect * projectedDistance),
                          viewPosition.y * focalLength / projectedDistance);
    float nearMask = distance <= 50.0 ? 1.0 : 0.0;
    float ringFlag = float(inIsRing);
    float ringFactor = mix(mix(1.0, 0.8, nearMask), 1.0, ringFlag);
    float pointSize = inPosition.w * 350.0 * 0.55 / max(distance, 0.1) * (uScreenHeight / 1080.0) * ringFactor *
                      pow(max(uPixelRatio, 0.0001), 0.8);
    float pixelSize = clamp(pointSize, 0.0, 300.0 * (uScreenHeight / 1080.0));
    vec2 ndcOffset = corner * (pixelSize * 0.5) * vec2(2.0 / (uAspect * uScreenHeight), 2.0 / uScreenHeight);
    gl_Position = vec4(ndcCenter + ndcOffset, 0.0, 1.0);
    vColor = vec4(float((inColor >> 0u) & 255u), float((inColor >> 8u) & 255u),
                  float((inColor >> 16u) & 255u), float((inColor >> 24u) & 255u)) / 255.0;
    vDistance = distance;
    vScale = uScale;
    vIsRing = ringFlag;
    vDensityCompensation = uDensityCompensation;
    vPointCoord = corner * 0.5 + 0.5;
}
