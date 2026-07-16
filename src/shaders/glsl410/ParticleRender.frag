#version 410 core

in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 point = gl_PointCoord * 2.0 - 1.0;
    float alpha = smoothstep(1.0, 0.0, dot(point, point));
    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
