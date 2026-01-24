#version 450

layout(std140, set=0, binding=0) uniform StarConstants
{
    vec4 uViewRow0;
    vec4 uViewRow1;
    vec4 uViewRow2;
    vec4 uViewRow3;

    vec4 uProjRow0;
    vec4 uProjRow1;
    vec4 uProjRow2;
    vec4 uProjRow3;

    vec4 uModelRow0;
    vec4 uModelRow1;
    vec4 uModelRow2;
    vec4 uModelRow3;

    vec4 uViewportParams; // x = 2/width, y = 2/height
    vec4 uTimeParams;     // x = timeSeconds
};

layout(location = 0) in vec3 inPos;     // world
layout(location = 1) in vec3 inColor;
layout(location = 2) in float inSize;
layout(location = 3) in float inSeed;

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vSeed;
layout(location = 2) out vec2 vUV;

vec4 mulRows(vec4 v, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
{
    return vec4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

void main()
{
    vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0),
        vec2(-1.0, -1.0),
        vec2( 1.0,  1.0),
        vec2( 1.0, -1.0)
    );

    vec2 corner = corners[gl_VertexIndex % 6];
    vec2 uv     = corner * 0.5 + 0.5;

    vec4 worldPos = mulRows(vec4(inPos, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    vec4 viewPos  = mulRows(worldPos,          uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);
    vec4 clipPos  = mulRows(viewPos,           uProjRow0,  uProjRow1,  uProjRow2,  uProjRow3);

    if (viewPos.z >= -0.001)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vColor = vec3(0.0);
        vSeed  = 0.0;
        vUV    = vec2(0.0);
        return;
    }

    float invZ = 1.0 / (-viewPos.z);
    float px   = clamp(inSize * (1000.0 * invZ), 1.0, 8.0);

    vec2 ndcCenter = clipPos.xy / clipPos.w;
    vec2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    vec2 ndcPos    = ndcCenter + ndcOffset;

    gl_Position = vec4(ndcPos * clipPos.w, clipPos.z, clipPos.w);

    vColor = inColor;
    vSeed  = inSeed;
    vUV    = uv;
}
