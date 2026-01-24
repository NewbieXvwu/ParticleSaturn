#version 450

layout(std140, set=0, binding=0) uniform SevenSegCB
{
    mat4 Projection;
    vec4 Transform; // x, y, scaleX, scaleY
    vec4 Color;     // rgb + pad
};

layout(location = 0) in vec2 inPos;
layout(location = 0) out vec3 vColor;

void main()
{
    vec2 worldPos = inPos * Transform.zw + Transform.xy;
    gl_Position = Projection * vec4(worldPos, 0.0, 1.0);
    vColor = Color.rgb;
}
