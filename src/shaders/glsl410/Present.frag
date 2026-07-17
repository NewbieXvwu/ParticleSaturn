#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uScene;

void main() {
    outColor = texture(uScene, vTexCoord);
}
