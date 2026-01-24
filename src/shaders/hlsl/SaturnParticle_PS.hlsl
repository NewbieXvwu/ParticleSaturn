struct PSIn
{
    float4 Pos   : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float3 vColor       : TEXCOORD1;
    float  vDist        : TEXCOORD2;
    float  vOpacity     : TEXCOORD3;
    float  vScaleFactor : TEXCOORD4;
    float  vIsRing      : TEXCOORD5;
};

cbuffer ParticleConstants
{
    float4 uViewRow0;
    float4 uViewRow1;
    float4 uViewRow2;
    float4 uViewRow3;

    float4 uProjRow0;
    float4 uProjRow1;
    float4 uProjRow2;
    float4 uProjRow3;

    float4 uModelRow0;
    float4 uModelRow1;
    float4 uModelRow2;
    float4 uModelRow3;

    float4 uViewportParams;
    float4 uTimeParams;
    float4 uRenderParams; // w = uDensityComp
};

float SmoothStep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = saturate(t);
    return t * t * (3.0 - 2.0 * t);
}

float4 main(PSIn i) : SV_Target
{
    float2 c = 2.0 * i.UV - 1.0;
    float  rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float glow = SmoothStep(1.0, 0.4, rr);

    float t = clamp((i.vScaleFactor - 0.15) * 0.4255, 0.0, 1.0);
    float tSmooth = SmoothStep(0.1, 0.9, t);

    float3 baseColor  = lerp(float3(0.35, 0.22, 0.05), i.vColor, tSmooth);
    float3 finalColor = baseColor * (0.2 + t);

    float closeMix = SmoothStep(40.0, 0.0, i.vDist);
    float3 closeRingColor = finalColor + float3(0.15, 0.12, 0.1) * closeMix;
    float3 closeBodyColor = lerp(finalColor, pow(i.vColor, float3(1.4, 1.4, 1.4)) * 1.5, closeMix * 0.8);
    finalColor = lerp(closeBodyColor, closeRingColor, i.vIsRing);

    float depthAlpha = SmoothStep(0.0, 10.0, i.vDist);
    float densityComp = uRenderParams.w;
    float finalAlpha = glow * i.vOpacity * (0.25 + 0.45 * SmoothStep(0.0, 0.5, t)) * depthAlpha * densityComp;

    return float4(finalColor, finalAlpha);
}
