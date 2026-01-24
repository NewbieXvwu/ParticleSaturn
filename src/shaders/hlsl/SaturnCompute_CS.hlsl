struct ParticleData
{
    float4 pos;
    uint   color;
    float  speed;
    float  isRing;
    float  pad;
};

StructuredBuffer<ParticleData>   g_ParticlesIn;
RWStructuredBuffer<ParticleData> g_ParticlesOut;

cbuffer ComputeConstants
{
    float uDt;
    float uHandScale;
    float uHandHas;
    uint  uParticleCount;
};

groupshared float s_timeFactor;
groupshared float s_bodyAngleCos;
groupshared float s_bodyAngleSin;
groupshared float s_dtScaled;

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    uint id = DTid.x;

    if (GTid.x == 0)
    {
        s_timeFactor   = lerp(1.0, uHandScale, uHandHas);
        float bodyAngle = 0.03 * uDt * s_timeFactor;
        s_bodyAngleCos = cos(bodyAngle);
        s_bodyAngleSin = sin(bodyAngle);
        s_dtScaled     = 0.2 * uDt * s_timeFactor;
    }
    GroupMemoryBarrierWithGroupSync();

    if (id >= uParticleCount)
        return;

    ParticleData p = g_ParticlesIn[id];

    float c, s;
    if (p.isRing < 0.5)
    {
        c = s_bodyAngleCos;
        s = s_bodyAngleSin;
    }
    else
    {
        float angle = p.speed * s_dtScaled;
        c = cos(angle);
        s = sin(angle);
    }

    g_ParticlesOut[id].pos.x = p.pos.x * c - p.pos.z * s;
    g_ParticlesOut[id].pos.y = p.pos.y;
    g_ParticlesOut[id].pos.z = p.pos.x * s + p.pos.z * c;
    g_ParticlesOut[id].pos.w = p.pos.w;
    g_ParticlesOut[id].color = p.color;
    g_ParticlesOut[id].speed = p.speed;
    g_ParticlesOut[id].isRing = p.isRing;
    g_ParticlesOut[id].pad = p.pad;
}
