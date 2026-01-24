#version 450

struct ParticleData
{
    vec4  pos;
    uint  color;
    float speed;
    float isRing;
    float pad;
};

// Vulkan 下建议显式指定 set/binding，避免不同驱动/编译器对默认绑定行为不一致。
layout(set=0, binding=0, std430) readonly buffer g_Particles
{
    ParticleData particles[];
};

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
    vec4 uRenderParams; // x=uScale, y=uPixelRatio, z=uScreenHeight, w=uDensityComp
};

vec4 mulRows(vec4 v, vec4 r0, vec4 r1, vec4 r2, vec4 r3)
{
    return vec4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vDist;
layout(location = 3) out float vOpacity;
layout(location = 4) out float vScaleFactor;
layout(location = 5) out float vIsRing;

float smoothStep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float hash(float n)
{
    uint x = floatBitsToUint(n);
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = ((x >> 16u) ^ x) * 0x45d9f3bu;
    x = (x >> 16u) ^ x;
    return float(x) * (1.0 / 4294967296.0);
}

float fastSin(float x)
{
    x = mod(x, 6.28318530718);
    x = x > 3.14159265359 ? x - 6.28318530718 : x;
    float x2 = x * x;
    return x * (1.0 - x2 * (0.16666667 - x2 * (0.00833333 - x2 * 0.0001984)));
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

    ParticleData p = particles[gl_InstanceIndex];

    float uScale        = uRenderParams.x;
    float uPixelRatio   = uRenderParams.y;
    float uScreenHeight = uRenderParams.z;

    vec4 col;
    col.r = float( p.color        & 0xFFu) / 255.0;
    col.g = float((p.color >>  8) & 0xFFu) / 255.0;
    col.b = float((p.color >> 16) & 0xFFu) / 255.0;
    col.a = float((p.color >> 24) & 0xFFu) / 255.0;

    vec4 worldPos = mulRows(vec4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    vec4 mvPosition  = mulRows(worldPos,                   uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);

    float dist = -mvPosition.z;

    if (mvPosition.z >= -0.001)
    {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUV    = vec2(0.0);
        vColor = vec3(0.0);
        vDist = 0.0;
        vOpacity = 0.0;
        vScaleFactor = 0.0;
        vIsRing = 0.0;
        return;
    }

    float chaosThreshold = 25.0;
    float chaosIntensity = smoothStep(chaosThreshold, 0.1, dist);
    chaosIntensity = chaosIntensity * chaosIntensity * chaosIntensity;

    vec3 noiseVec = vec3(0.0);
    if (chaosIntensity > 0.001)
    {
        float highFreqTime = uTimeParams.x * 40.0;
        vec3 posScaled = p.pos.xyz * 10.0;
        float hashX = hash(p.pos.y * 43758.5) * 0.5;
        float hashY = hash(p.pos.x * 43758.5) * 0.5;
        float hashZ = hash(p.pos.z * 43758.5) * 0.5;
        noiseVec = vec3(
            fastSin(highFreqTime + posScaled.x) * hashX,
            fastSin(highFreqTime + posScaled.y + 1.5708) * hashY,
            fastSin(highFreqTime * 0.5) * hashZ
        ) * 3.0;
    }
    mvPosition.xyz = mix(mvPosition.xyz, mvPosition.xyz + noiseVec, chaosIntensity);

    vec4 clipPos  = mulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);

    float invDist = 1.0 / max(dist, 0.1);
    float basePointSize = p.pos.w * 350.0 * invDist * 0.55;
    float screenScale = uScreenHeight / 1080.0;
    float pointSize = basePointSize * screenScale;
    float nearMask = (dist <= 50.0) ? 1.0 : 0.0;
    float ringFactor = mix(mix(1.0, 0.8, nearMask), 1.0, p.isRing);
    pointSize *= ringFactor * pow(max(uPixelRatio, 0.0001), 0.8);
    float px = clamp(pointSize, 0.0, 300.0 * screenScale);

    vec2 ndcCenter = clipPos.xy / clipPos.w;
    vec2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    vec2 ndcPos    = ndcCenter + ndcOffset;

    gl_Position = vec4(ndcPos * clipPos.w, clipPos.z, clipPos.w);

    vUV = corner * 0.5 + 0.5;
    vColor = col.rgb;
    vDist  = dist;
    vOpacity = col.a;
    vScaleFactor = uScale;
    vIsRing = p.isRing;
}
