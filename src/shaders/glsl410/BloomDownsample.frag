#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uSource;
uniform vec2 uTexelSize;
uniform float uThreshold;

void main() {
    vec2 halfPixel = uTexelSize * 0.5;
    vec3 color = texture(uSource, vTexCoord + vec2(-halfPixel.x, -halfPixel.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(halfPixel.x, -halfPixel.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(-halfPixel.x, halfPixel.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(halfPixel.x, halfPixel.y)).rgb;
    color *= 0.25;
    if (uThreshold > 0.0001) {
        float maximum = max(color.r, max(color.g, color.b));
        color *= smoothstep(uThreshold, uThreshold * 2.0, maximum);
    }
    outColor = vec4(color, 1.0);
}
