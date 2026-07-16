#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomStrength;

void main() {
    vec3 color = texture(uScene, vTexCoord).rgb + texture(uBloom, vTexCoord).rgb * uBloomStrength;
    float maximum = max(color.r, max(color.g, color.b));
    if (maximum >= 1.0) color = mix(color, color / (color + vec3(1.0)), 0.5);
    outColor = vec4(color, 1.0);
}
