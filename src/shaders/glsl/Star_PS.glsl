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

    vec4 uViewportParams;
    vec4 uTimeParams; // x = timeSeconds
};

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vSeed;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 oColor;

void main()
{
    vec2 c = 2.0 * vUV - 1.0;
    float rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float n  = fract(sin(dot(gl_FragCoord.xy, vec2(12.9, 78.2))) * 43758.5);
    float tw = 0.7 + 0.3 * sin(uTimeParams.x * 2.0 + (n + vSeed) * 10.0);

    vec3 col = vColor * tw * 3.0;
    float a  = pow(1.0 - rr, 1.5) * 0.9;
    oColor   = vec4(col, a);
}
