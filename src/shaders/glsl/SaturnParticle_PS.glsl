#version 450

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vDist;
layout(location = 3) in float vOpacity;
layout(location = 4) in float vScaleFactor;
layout(location = 5) in float vIsRing;

layout(set=0, binding=1, std140) uniform ParticleConstants
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
    vec4 uTimeParams;
    vec4 uRenderParams; // w=uDensityComp
};

layout(location = 0) out vec4 oColor;

float smoothStep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main()
{
    vec2 c = 2.0 * vUV - 1.0;
    float rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float glow = smoothStep(1.0, 0.4, rr);

    float t = clamp((vScaleFactor - 0.15) * 0.4255, 0.0, 1.0);
    float tSmooth = smoothStep(0.1, 0.9, t);

    vec3 baseColor = mix(vec3(0.35, 0.22, 0.05), vColor, tSmooth);
    vec3 finalColor = baseColor * (0.2 + t);

    float closeMix = smoothStep(40.0, 0.0, vDist);
    vec3 closeRingColor = finalColor + vec3(0.15, 0.12, 0.1) * closeMix;
    vec3 closeBodyColor = mix(finalColor, pow(vColor, vec3(1.4)) * 1.5, closeMix * 0.8);
    finalColor = mix(closeBodyColor, closeRingColor, vIsRing);

    float depthAlpha = smoothStep(0.0, 10.0, vDist);
    float densityComp = uRenderParams.w;
    float finalAlpha = glow * vOpacity * (0.25 + 0.45 * smoothStep(0.0, 0.5, t)) * depthAlpha * densityComp;

    oColor = vec4(finalColor, finalAlpha);
}
