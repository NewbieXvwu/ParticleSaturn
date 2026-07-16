#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uScene;
uniform sampler2D uBloom;

void main() {
    vec3 color = texture(uScene, vTexCoord).rgb + texture(uBloom, vTexCoord).rgb;
    color = (color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14);
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
