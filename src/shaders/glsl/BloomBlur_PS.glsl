#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set=0, binding=0) uniform sampler2D g_Texture;
layout(std140, set=0, binding=1) uniform BlurCB
{
    vec2  g_TexelSize;
    float g_Offset;
    float g_Threshold;
};

void main()
{
    vec2 off = g_TexelSize * (g_Offset + 0.5);
    vec3 sum = texture(g_Texture, vUV + vec2(-off.x,  off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2( off.x,  off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2( off.x, -off.y)).rgb;
    sum     += texture(g_Texture, vUV + vec2(-off.x, -off.y)).rgb;
    oColor = vec4(sum * 0.25, 1.0);
}
