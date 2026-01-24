#version 450

layout(location = 0) out vec2 vUV;

void main()
{
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0, -1.0),
        vec2( 1.0,  1.0)
    );

    const vec2 uv[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 1.0),
        vec2(1.0, 0.0)
    );

    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV         = uv[gl_VertexIndex];
}
