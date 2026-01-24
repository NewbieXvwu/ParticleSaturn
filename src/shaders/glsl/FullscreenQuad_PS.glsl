#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// Vulkan 下显式 set/binding，避免不同驱动默认绑定行为差异。
layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(set=0, binding=1) uniform sampler2D g_BloomTexture;

layout(std140, set=0, binding=2) uniform BloomCB
{
    float g_BloomStrength;
    float g_Transparent;
    vec2 _pad;
};

vec3 toneMap(vec3 hdr)
{
    return hdr / (hdr + vec3(1.0));
}

void main()
{
    vec3 col = texture(g_Texture, vUV).rgb;
    vec3 bloom = texture(g_BloomTexture, vUV).rgb;

    // Bloom: 叠加模糊后的高光
    col = col + bloom * g_BloomStrength;

    float maxRGB = max(max(col.r, col.g), col.b);
    float w = (maxRGB >= 1.0) ? 0.5 : 0.0;
    col = mix(col, toneMap(col), w);

    float alpha = mix(1.0, maxRGB, g_Transparent);
    // DirectComposition/DWM 要求预乘 alpha（premultiplied alpha）
    oColor = vec4(col * alpha, alpha);
}
