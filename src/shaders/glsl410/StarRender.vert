#version 410 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inSize;
layout(location = 3) in float inRandomSeed;

uniform float uTime;
uniform float uAspect;

out vec4 vColor;
out float vTime;

void main() {
    float rotation = uTime * 0.005;
    vec3 position = vec3(inPosition.x * cos(rotation) + inPosition.z * sin(rotation), inPosition.y,
                         -inPosition.x * sin(rotation) + inPosition.z * cos(rotation));
    float distance = max(100.0 - position.z, 0.001);
    float focalLength = 1.0 / tan(1.047 * 0.5);
    gl_Position = vec4(position.x * focalLength / (uAspect * distance), position.y * focalLength / distance, 0.0, 1.0);
    gl_PointSize = clamp(inSize * 1000.0 / distance, 1.0, 8.0);
    vColor = vec4(inColor, inRandomSeed);
    vTime = uTime;
}
