#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uSource;
uniform vec2 uTexelSize;
uniform float uOffset;

void main() {
    vec2 offset = uTexelSize * (uOffset + 0.5);
    vec3 color = texture(uSource, vTexCoord + vec2(-offset.x, -offset.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(offset.x, -offset.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(-offset.x, offset.y)).rgb;
    color += texture(uSource, vTexCoord + vec2(offset.x, offset.y)).rgb;
    outColor = vec4(color * 0.25, 1.0);
}
