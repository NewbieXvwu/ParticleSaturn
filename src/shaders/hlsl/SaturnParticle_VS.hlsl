struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

StructuredBuffer<ParticleData> g_Particles;

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

    float4 uViewportParams; // x=2/width, y=2/height, z=width, w=height
    float4 uTimeParams;     // x=time
    float4 uRenderParams;   // x=uScale, y=uPixelRatio, z=uScreenHeight, w=uDensityComp
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float4 UnpackRGBA8(uint c)
{
    float4 o;
    o.x = float((c      ) & 0xFFu) / 255.0;
    o.y = float((c >>  8) & 0xFFu) / 255.0;
    o.z = float((c >> 16) & 0xFFu) / 255.0;
    o.w = float((c >> 24) & 0xFFu) / 255.0;
    return o;
}

float SmoothStep(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = saturate(t);
    return t * t * (3.0 - 2.0 * t);
}

float Hash(float n)
{
    uint x = asuint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

float FastSin(float x)
{
    const float TwoPi = 6.28318530718;
    const float Pi    = 3.14159265359;

    x = fmod(x, TwoPi);
    x = (x > Pi) ? (x - TwoPi) : x;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * (0.00833333 - x2 * 0.0001984)));
}

struct VSOut
{
    float4 Pos   : SV_POSITION;
    float2 UV    : TEXCOORD0;
    float3 vColor       : TEXCOORD1;
    float  vDist        : TEXCOORD2;
    float  vOpacity     : TEXCOORD3;
    float  vScaleFactor : TEXCOORD4;
    float  vIsRing      : TEXCOORD5;
};

VSOut main(uint VertId : SV_VertexID, uint InstId : SV_InstanceID)
{
    float2 corners[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0),

        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0)
    };

    float2 corner = corners[VertId % 6];

    ParticleData p = g_Particles[InstId];
    float4 col = UnpackRGBA8(p.color);

    const float uScale       = uRenderParams.x;
    const float uPixelRatio  = uRenderParams.y;
    const float uScreenHeight= uRenderParams.z;

    float4 worldPos = MulRows(float4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 mvPosition  = MulRows(worldPos,                    uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);

    float dist = -mvPosition.z;

    // 与 OpenGL 旧逻辑一致：只接受 viewPos.z < 0 的点（相机前方）。
    if (mvPosition.z >= -0.001)
    {
        VSOut o;
        o.Pos = float4(2.0, 2.0, 2.0, 1.0);
        o.UV  = float2(0.0, 0.0);
        o.vColor = float3(0.0, 0.0, 0.0);
        o.vDist = 0.0;
        o.vOpacity = 0.0;
        o.vScaleFactor = 0.0;
        o.vIsRing = 0.0;
        return o;
    }

    // Chaos（近距离扰动）
    float chaosThreshold = 25.0;
    float chaosIntensity = SmoothStep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    float3 noiseVec = float3(0.0, 0.0, 0.0);
    if (chaosIntensity > 0.001)
    {
        float highFreqTime = uTimeParams.x * 40.0;
        float3 posScaled   = p.pos.xyz * 10.0;
        float hashX = Hash(p.pos.y * 43758.5) * 0.5;
        float hashY = Hash(p.pos.x * 43758.5) * 0.5;
        float hashZ = Hash(p.pos.z * 43758.5) * 0.5;
        noiseVec = float3(
            FastSin(highFreqTime + posScaled.x) * hashX,
            FastSin(highFreqTime + posScaled.y + 1.5708) * hashY,
            FastSin(highFreqTime * 0.5) * hashZ
        ) * 3.0;
    }
    mvPosition.xyz = lerp(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    float4 clipPos  = MulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);

    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = p.pos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;

    // ringFactor：仅对本体粒子在近距离略缩小
    float nearMask = (dist <= 50.0) ? 1.0 : 0.0;
    float ringFactor = lerp(lerp(1.0, 0.8, nearMask), 1.0, p.isRing);
    pointSize *= ringFactor * pow(max(uPixelRatio, 0.0001), 0.8);

    float px = clamp(pointSize, 0.0, 300.0 * screenScale);

    float2 ndcCenter = clipPos.xy / clipPos.w;
    float2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    float2 ndcPos    = ndcCenter + ndcOffset;

    VSOut o;
    o.Pos   = float4(ndcPos * clipPos.w, clipPos.z, clipPos.w);
    o.UV    = corner * 0.5 + 0.5;
    o.vColor       = col.rgb;
    o.vDist        = dist;
    o.vOpacity     = col.a;
    o.vScaleFactor = uScale;
    o.vIsRing      = p.isRing;
    return o;
}
