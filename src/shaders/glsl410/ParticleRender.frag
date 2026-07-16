#version 410 core

in vec4 vColor;
in float vDistance;
in float vScale;
in float vIsRing;
in float vDensityCompensation;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 point = gl_PointCoord * 2.0 - 1.0;
    float radiusSquared = dot(point, point);
    if (radiusSquared > 1.0) discard;
    float glow = smoothstep(1.0, 0.4, radiusSquared);
    float t = clamp((vScale - 0.15) * 0.4255, 0.0, 1.0);
    float smoothedT = smoothstep(0.1, 0.9, t);
    vec3 color = mix(vec3(0.35, 0.22, 0.05), vColor.rgb, smoothedT) * (0.2 + t);
    float closeMix = smoothstep(40.0, 0.0, vDistance);
    vec3 ringColor = color + vec3(0.15, 0.12, 0.1) * closeMix;
    vec3 bodyColor = mix(color, pow(vColor.rgb, vec3(1.4)) * 1.5, closeMix * 0.8);
    color = mix(bodyColor, ringColor, vIsRing);
    float depthAlpha = smoothstep(0.0, 10.0, vDistance);
    float alpha = glow * vColor.a * (0.25 + 0.45 * smoothstep(0.0, 0.5, t)) * depthAlpha * vDensityCompensation;
    outColor = vec4(color, alpha);
}
