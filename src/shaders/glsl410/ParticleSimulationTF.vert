#version 410 core

layout(location = 0) in vec4 inPosition;
layout(location = 1) in uint inColor;
layout(location = 2) in float inSpeed;
layout(location = 3) in float inIsRing;
layout(location = 4) in float inPadding;

uniform float uDeltaTime;
uniform float uHandScale;
uniform float uHandTracked;

out vec4 tfPosition;
flat out uint tfColor;
out float tfSpeed;
out float tfIsRing;
out float tfPadding;

void main() {
    float scale = mix(1.0, uHandScale, uHandTracked);
    float angle = (inIsRing < 0.5 ? 0.03 : inSpeed * 0.2) * uDeltaTime * scale;
    float c = cos(angle);
    float s = sin(angle);
    tfPosition = vec4(inPosition.x * c - inPosition.z * s, inPosition.y,
                      inPosition.x * s + inPosition.z * c, inPosition.w);
    tfColor = inColor;
    tfSpeed = inSpeed;
    tfIsRing = inIsRing;
    tfPadding = inPadding;
}
