#version 450

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

struct ParticleData
{
    vec4  pos;
    uint  color;
    float speed;
    float isRing;
    float pad;
};

layout(set=0, binding=0, std430) readonly buffer g_ParticlesIn
{
    ParticleData particlesIn[];
};

layout(set=0, binding=1, std430) writeonly buffer g_ParticlesOut
{
    ParticleData particlesOut[];
};

layout(set=0, binding=2, std140) uniform ComputeConstants
{
    float uDt;
    float uHandScale;
    float uHandHas;
    uint  uParticleCount;
};

shared float s_timeFactor;
shared float s_bodyAngleCos;
shared float s_bodyAngleSin;
shared float s_dtScaled;

void main()
{
    uint id = gl_GlobalInvocationID.x;

    if (gl_LocalInvocationID.x == 0u)
    {
        s_timeFactor    = mix(1.0, uHandScale, uHandHas);
        float bodyAngle = 0.03 * uDt * s_timeFactor;
        s_bodyAngleCos  = cos(bodyAngle);
        s_bodyAngleSin  = sin(bodyAngle);
        s_dtScaled      = 0.2 * uDt * s_timeFactor;
    }
    barrier();

    if (id >= uParticleCount)
        return;

    ParticleData p = particlesIn[id];

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

    particlesOut[id].pos.x = p.pos.x * c - p.pos.z * s;
    particlesOut[id].pos.y = p.pos.y;
    particlesOut[id].pos.z = p.pos.x * s + p.pos.z * c;
    particlesOut[id].pos.w = p.pos.w;
    particlesOut[id].color = p.color;
    particlesOut[id].speed = p.speed;
    particlesOut[id].isRing = p.isRing;
    particlesOut[id].pad = p.pad;
}
