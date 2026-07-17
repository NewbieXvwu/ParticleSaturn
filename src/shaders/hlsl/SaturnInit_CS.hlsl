#include "Particle.hlsl"
typedef Particle ParticleData;

RWStructuredBuffer<ParticleData> g_ParticlesOut;

cbuffer InitConstants
{
    uint  uParticleCount;
    uint  uSeed;
    float uRadius;
    float _pad;
};

// PCG 伪随机数生成器
uint PCGHash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random01(inout uint state)
{
    state = PCGHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

uint PackRGBA8(float r, float g, float b, float a)
{
    uint ir = uint(saturate(r) * 255.0 + 0.5);
    uint ig = uint(saturate(g) * 255.0 + 0.5);
    uint ib = uint(saturate(b) * 255.0 + 0.5);
    uint ia = uint(saturate(a) * 255.0 + 0.5);
    return ir | (ig << 8) | (ib << 16) | (ia << 24);
}

float3 HexToRGB(uint hex)
{
    float r = float((hex >> 16) & 0xFF) / 255.0;
    float g = float((hex >> 8) & 0xFF) / 255.0;
    float b = float(hex & 0xFF) / 255.0;
    return float3(r, g, b);
}

float3 Mix(float3 a, float3 b, float t)
{
    return a * (1.0 - t) + b * t;
}

[numthreads(256, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint id = DTid.x;
    if (id >= uParticleCount)
        return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;
    float typeRnd = Random01(rngState);

    float4 pos = float4(0, 0, 0, 1);
    float3 colRGB = float3(1, 1, 1);
    float alpha = 1.0;
    float speed = 0.0;
    uint isRing = 0u;

    float R = uRadius;

    if (typeRnd < 0.25)
    {
        // --- 土星本体粒子 ---
        float th = 6.28318 * Random01(rngState);
        float ph = acos(2.0 * Random01(rngState) - 1.0);

        pos.x = R * sin(ph) * cos(th);
        pos.y = R * cos(ph) * 0.9;
        pos.z = R * sin(ph) * sin(th);

        float lat = (pos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4;
        if (ci < 0) ci = 0;

        float3 cols[4];
        cols[0] = HexToRGB(0xE3DAC5);
        cols[1] = HexToRGB(0xC9A070);
        cols[2] = HexToRGB(0xE3DAC5);
        cols[3] = HexToRGB(0xB08D55);
        colRGB = cols[ci];

        pos.w = 1.0 + Random01(rngState) * 0.8;
        alpha = 0.8;
        speed = 0.0;
        isRing = 0u;
    }
    else
    {
        // --- 土星环粒子 ---
        float z = Random01(rngState);
        float rad = 0.0;
        float3 c = float3(1, 1, 1);
        float s = 1.0;
        float o = 1.0;

        if (z < 0.15)
        {
            rad = R * (1.235 + Random01(rngState) * 0.29);
            c = HexToRGB(0x2A2520);
            s = 0.5;
            o = 0.3;
        }
        else if (z < 0.65)
        {
            float t = Random01(rngState);
            rad = R * (1.525 + t * 0.425);
            c = Mix(HexToRGB(0xCDBFA0), HexToRGB(0xDCCBBA), t);
            s = 0.8 + Random01(rngState) * 0.6;
            o = 0.85;
            if (sin(rad * 2.0) > 0.8)
                o *= 1.2;
        }
        else if (z < 0.69)
        {
            rad = R * (1.95 + Random01(rngState) * 0.075);
            c = HexToRGB(0x050505);
            s = 0.3;
            o = 0.1;
        }
        else if (z < 0.99)
        {
            rad = R * (2.025 + Random01(rngState) * 0.245);
            c = HexToRGB(0x989085);
            s = 0.7;
            o = 0.6;
            if (rad > R * 2.2 && rad < R * 2.21)
                o = 0.1;
        }
        else
        {
            rad = R * (2.32 + Random01(rngState) * 0.02);
            c = HexToRGB(0xAFAFA0);
            s = 1.0;
            o = 0.7;
        }

        float th = Random01(rngState) * 6.28318;
        pos.x = rad * cos(th);
        pos.z = rad * sin(th);

        float heightRange = (rad > R * 2.3) ? 0.4 : 0.15;
        pos.y = (Random01(rngState) - 0.5) * heightRange;

        colRGB = c;
        pos.w = s;
        alpha = o;
        speed = 8.0 / sqrt(rad);
        isRing = 1u;
    }

    ParticleData p;
    p.position = pos;
    p.color = PackRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.padding = 0u;
    g_ParticlesOut[id] = p;
}
