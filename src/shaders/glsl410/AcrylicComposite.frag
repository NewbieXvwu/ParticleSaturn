#version 410 core

in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

uniform sampler2D uSource;
uniform vec3 uTint;
uniform float uBaseOpacity;
uniform float uSaturation;
uniform float uAdaptive;
uniform float uDarkMode;
uniform float uExclusion;

void main() {
    vec3 color = texture(uSource, vTexCoord).rgb;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = clamp(vec3(luminance) + (color - vec3(luminance)) * uSaturation, 0.0, 1.0);
    float adjustedLuminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float opacity = uDarkMode > 0.5
        ? clamp(uBaseOpacity + (adjustedLuminance - 0.5) * uAdaptive, 0.0, 1.0)
        : clamp(uBaseOpacity + (0.5 - adjustedLuminance) * uAdaptive, 0.0, 1.0);
    vec3 exclusion = (color + uTint) - (2.0 * color * uTint);
    vec3 mixed = mix(color, exclusion, clamp(uExclusion, 0.0, 1.0));
    outColor = vec4(mix(color, mixed, opacity), 1.0);
}
