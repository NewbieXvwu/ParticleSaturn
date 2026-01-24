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

 vec3 brightPass(vec3 hdr)
 {
     float m = max(hdr.r, max(hdr.g, hdr.b));
     // g_Threshold<=0 时不做高光提取：避免 smoothstep(edge0==edge1) 的未定义行为。
     if (g_Threshold <= 0.0001)
         return hdr;
     float w = smoothstep(g_Threshold, g_Threshold * 2.0, m);
     return hdr * w;
 }

void main()
{
    vec2 halfPix = g_TexelSize * 0.5;
    vec3 c0 = texture(g_Texture, vUV + vec2(-halfPix.x, -halfPix.y)).rgb;
    vec3 c1 = texture(g_Texture, vUV + vec2( halfPix.x, -halfPix.y)).rgb;
    vec3 c2 = texture(g_Texture, vUV + vec2(-halfPix.x,  halfPix.y)).rgb;
    vec3 c3 = texture(g_Texture, vUV + vec2( halfPix.x,  halfPix.y)).rgb;
    vec3 col = (c0 + c1 + c2 + c3) * 0.25;
    col = brightPass(col);
    oColor = vec4(col, 1.0);
}
