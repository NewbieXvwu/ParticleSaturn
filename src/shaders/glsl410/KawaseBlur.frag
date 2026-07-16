#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uSource;
uniform vec2 uTexelSize;

void main() {
    vec3 color = texture(uSource, vTexCoord + uTexelSize * vec2(-1.5, -1.5)).rgb;
    color += texture(uSource, vTexCoord + uTexelSize * vec2(1.5, -1.5)).rgb;
    color += texture(uSource, vTexCoord + uTexelSize * vec2(-1.5, 1.5)).rgb;
    color += texture(uSource, vTexCoord + uTexelSize * vec2(1.5, 1.5)).rgb;
    outColor = vec4(color * 0.25, 1.0);
}
