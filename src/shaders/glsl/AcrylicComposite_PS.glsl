#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(std140, set=0, binding=1) uniform AcrylicCB
{
    vec4 g_Tint;   // rgb + baseOpacity
    vec4 g_Params; // x=saturation, y=adaptive, z=darkModeFlag, w=exclusionStrength
};

vec3 applySaturation(vec3 c, float sat)
{
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 gray = vec3(lum);
    return clamp(gray + (c - gray) * sat, 0.0, 1.0);
}

void main()
{
    vec3 col = texture(g_Texture, vUV).rgb;
    col = applySaturation(col, g_Params.x);

    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    float a   = g_Tint.a;
    float adapt = g_Params.y;
    if (g_Params.z > 0.5) // dark
        a = clamp(a + (lum - 0.5) * adapt, 0.0, 1.0);
    else // light
        a = clamp(a + (0.5 - lum) * adapt, 0.0, 1.0);

    vec3 tint = g_Tint.rgb;
    vec3 excl = (col + tint) - (2.0 * col * tint);
    vec3 mixed = mix(col, excl, clamp(g_Params.w, 0.0, 1.0));
    vec3 outCol = mix(col, mixed, a);

    oColor = vec4(outCol, 1.0);
}
