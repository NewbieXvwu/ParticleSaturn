#version 410 core

in vec4 vColor;
in float vTime;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 point = gl_PointCoord * 2.0 - 1.0;
    float radiusSquared = dot(point, point);
    if (radiusSquared > 1.0) discard;
    float noise = fract(sin(dot(gl_FragCoord.xy, vec2(12.9, 78.2))) * 43758.5);
    float twinkle = 0.7 + 0.3 * sin(vTime * 2.0 + (noise + vColor.a) * 10.0);
    outColor = vec4(vColor.rgb * twinkle * 3.0, pow(1.0 - radiusSquared, 1.5) * 0.9);
}
