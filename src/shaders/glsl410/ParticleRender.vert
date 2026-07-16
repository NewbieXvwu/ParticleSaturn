#version 410 core

layout(location = 0) in vec4 inPosition;
layout(location = 1) in uint inColor;

uniform mat4 uViewProjection;

out vec4 vColor;

void main() {
    gl_Position = uViewProjection * vec4(inPosition.xyz, 1.0);
    gl_PointSize = max(1.0, inPosition.w);
    vColor = vec4(float((inColor >> 0u) & 255u), float((inColor >> 8u) & 255u),
                  float((inColor >> 16u) & 255u), float((inColor >> 24u) & 255u)) / 255.0;
}
