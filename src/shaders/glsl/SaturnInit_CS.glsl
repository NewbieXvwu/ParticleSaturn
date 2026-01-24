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

layout(set=0, binding=0, std430) writeonly buffer g_ParticlesOut
{
    ParticleData particlesOut[];
};

layout(set=0, binding=1, std140) uniform InitConstants
{
    uint  uParticleCount;
    uint  uSeed;
    float uRadius;
    float _pad;
};

// PCG 伪随机数生成器
uint pcgHash(uint inp)
{
    uint state = inp * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random01(inout uint state)
{
    state = pcgHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

uint packRGBA8(float r, float g, float b, float a)
{
    uint ir = uint(clamp(r, 0.0, 1.0) * 255.0 + 0.5);
    uint ig = uint(clamp(g, 0.0, 1.0) * 255.0 + 0.5);
    uint ib = uint(clamp(b, 0.0, 1.0) * 255.0 + 0.5);
    uint ia = uint(clamp(a, 0.0, 1.0) * 255.0 + 0.5);
    return ir | (ig << 8u) | (ib << 16u) | (ia << 24u);
}

vec3 hexToRGB(uint hex)
{
    float r = float((hex >> 16u) & 0xFFu) / 255.0;
    float g = float((hex >> 8u) & 0xFFu) / 255.0;
    float b = float(hex & 0xFFu) / 255.0;
    return vec3(r, g, b);
}

void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount)
        return;

    uint rngState = id * 1973u + uSeed * 9277u + 26699u;
    float typeRnd = random01(rngState);

    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0);
    vec3 colRGB = vec3(1.0);
    float alpha = 1.0;
    float speed = 0.0;
    float isRing = 0.0;

    float R = uRadius;

    if (typeRnd < 0.25)
    {
        // --- 土星本体粒子 ---
        float th = 6.28318 * random01(rngState);
        float ph = acos(2.0 * random01(rngState) - 1.0);

        pos.x = R * sin(ph) * cos(th);
        pos.y = R * cos(ph) * 0.9;
        pos.z = R * sin(ph) * sin(th);

        float lat = (pos.y / 0.9 / R + 1.0) * 0.5;
        int idxInt = int(lat * 4.0 + cos(lat * 40.0) * 0.8 + cos(lat * 15.0) * 0.4);
        int ci = idxInt - (idxInt / 4) * 4;
        if (ci < 0) ci = 0;

        vec3 cols[4];
        cols[0] = hexToRGB(0xE3DAC5u);
        cols[1] = hexToRGB(0xC9A070u);
        cols[2] = hexToRGB(0xE3DAC5u);
        cols[3] = hexToRGB(0xB08D55u);
        colRGB = cols[ci];

        pos.w = 1.0 + random01(rngState) * 0.8;
        alpha = 0.8;
        speed = 0.0;
        isRing = 0.0;
    }
    else
    {
        // --- 土星环粒子 ---
        float z = random01(rngState);
        float rad = 0.0;
        vec3 c = vec3(1.0);
        float s = 1.0;
        float o = 1.0;

        if (z < 0.15)
        {
            rad = R * (1.235 + random01(rngState) * 0.29);
            c = hexToRGB(0x2A2520u);
            s = 0.5;
            o = 0.3;
        }
        else if (z < 0.65)
        {
            float t = random01(rngState);
            rad = R * (1.525 + t * 0.425);
            c = mix(hexToRGB(0xCDBFA0u), hexToRGB(0xDCCBBAu), t);
            s = 0.8 + random01(rngState) * 0.6;
            o = 0.85;
            if (sin(rad * 2.0) > 0.8)
                o *= 1.2;
        }
        else if (z < 0.69)
        {
            rad = R * (1.95 + random01(rngState) * 0.075);
            c = hexToRGB(0x050505u);
            s = 0.3;
            o = 0.1;
        }
        else if (z < 0.99)
        {
            rad = R * (2.025 + random01(rngState) * 0.245);
            c = hexToRGB(0x989085u);
            s = 0.7;
            o = 0.6;
            if (rad > R * 2.2 && rad < R * 2.21)
                o = 0.1;
        }
        else
        {
            rad = R * (2.32 + random01(rngState) * 0.02);
            c = hexToRGB(0xAFAFA0u);
            s = 1.0;
            o = 0.7;
        }

        float th = random01(rngState) * 6.28318;
        pos.x = rad * cos(th);
        pos.z = rad * sin(th);

        float heightRange = (rad > R * 2.3) ? 0.4 : 0.15;
        pos.y = (random01(rngState) - 0.5) * heightRange;

        colRGB = c;
        pos.w = s;
        alpha = o;
        speed = 8.0 / sqrt(rad);
        isRing = 1.0;
    }

    ParticleData p;
    p.pos = pos;
    p.color = packRGBA8(colRGB.x, colRGB.y, colRGB.z, alpha);
    p.speed = speed;
    p.isRing = isRing;
    p.pad = 0.0;
    particlesOut[id] = p;
}
