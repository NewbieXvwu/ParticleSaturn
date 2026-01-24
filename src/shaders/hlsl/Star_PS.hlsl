cbuffer StarConstants
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
    float4 uTimeParams; // x = timeSeconds
};

struct PSIn
{
    float4 Pos   : SV_POSITION;
    float3 Color : COLOR0;
    float  Seed  : TEXCOORD0;
    float2 UV    : TEXCOORD1;
};

float4 main(PSIn i) : SV_Target
{
    float2 c = 2.0 * i.UV - 1.0;
    float  rr = dot(c, c);
    if (rr > 1.0)
        discard;

    float n  = frac(sin(dot(i.Pos.xy, float2(12.9, 78.2))) * 43758.5);
    float tw = 0.7 + 0.3 * sin(uTimeParams.x * 2.0 + (n + i.Seed) * 10.0);

    float3 col = i.Color * tw * 3.0;
    float  a   = pow(1.0 - rr, 1.5) * 0.9;
    return float4(col, a);
}
