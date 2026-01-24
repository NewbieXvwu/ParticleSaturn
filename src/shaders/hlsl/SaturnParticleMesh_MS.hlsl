#define GROUP_SIZE 32
#define VERTS_PER_PARTICLE 4
#define PRIMS_PER_PARTICLE 2
#define MAX_VERTS (GROUP_SIZE * VERTS_PER_PARTICLE)  // 128
#define MAX_PRIMS (GROUP_SIZE * PRIMS_PER_PARTICLE)  // 64

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

    float4 uViewportParams;
    float4 uTimeParams;
    float4 uRenderParams;

    uint   uParticleCount;
    uint3  _pad;
};

struct VertexOutput
{
    float4 Pos         : SV_POSITION;
    float2 UV          : TEXCOORD0;
    float3 vColor      : TEXCOORD1;
    float  vDist       : TEXCOORD2;
    float  vOpacity    : TEXCOORD3;
    float  vScaleFactor: TEXCOORD4;
    float  vIsRing     : TEXCOORD5;
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float4 UnpackRGBA8(uint c)
{
    return float4(
        float((c      ) & 0xFF) / 255.0,
        float((c >>  8) & 0xFF) / 255.0,
        float((c >> 16) & 0xFF) / 255.0,
        float((c >> 24) & 0xFF) / 255.0
    );
}

float SmoothStep(float edge0, float edge1, float x)
{
    float t = saturate((x - edge0) / (edge1 - edge0));
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

[outputtopology("triangle")]
[numthreads(GROUP_SIZE, 1, 1)]
void main(
    uint gtid : SV_GroupThreadID,
    uint gid  : SV_GroupID,
    out vertices VertexOutput verts[MAX_VERTS],
    out indices uint3 tris[MAX_PRIMS])
{
    uint particleIdx = gid * GROUP_SIZE + gtid;

    // 计算这个组实际要处理的粒子数
    uint groupStart = gid * GROUP_SIZE;
    uint groupEnd = min(groupStart + GROUP_SIZE, uParticleCount);
    uint particlesInGroup = groupEnd - groupStart;

    // 设置输出数量（所有线程必须调用相同的值）
    SetMeshOutputCounts(particlesInGroup * VERTS_PER_PARTICLE, particlesInGroup * PRIMS_PER_PARTICLE);

    // 超出粒子数量的线程不需要生成输出
    if (gtid >= particlesInGroup)
        return;

    ParticleData p = g_Particles[particleIdx];
    float4 col = UnpackRGBA8(p.color);

    float uScale        = uRenderParams.x;
    float uPixelRatio   = uRenderParams.y;
    float uScreenHeight = uRenderParams.z;

    float4 worldPos   = MulRows(float4(p.pos.xyz * uScale, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 mvPosition = MulRows(worldPos, uViewRow0, uViewRow1, uViewRow2, uViewRow3);

    float dist = -mvPosition.z;

    // 剔除相机后方的粒子
    bool valid = (mvPosition.z < -0.001);

    float3 vColor = float3(0, 0, 0);
    float vOpacity = 0.0;
    float vScaleFactor = 0.0;
    float4 clipPos = float4(2, 2, 2, 1);
    float halfSize = 0.0;

    if (valid)
    {
        // 点大小计算（与 Vertex Pulling VS 保持一致）
        float invDist = 1.0 / max(dist, 0.1);
        float basePointSize = p.pos.w * 350.0 * invDist * 0.55;
        float screenScale = uScreenHeight / 1080.0;
        float pointSize = basePointSize * screenScale;

        // ringFactor：仅对本体粒子在近距离略缩小（与 Vertex Pulling 一致）
        float nearMask = (dist <= 50.0) ? 1.0 : 0.0;
        float ringFactor = lerp(lerp(1.0, 0.8, nearMask), 1.0, p.isRing);
        float pixelFactor = pow(max(uPixelRatio, 0.0001), 0.8);
        pointSize *= ringFactor * pixelFactor;

        // 与 Vertex Pulling 一致的 clamp 范围
        float px = clamp(pointSize, 0.5, 300.0 * screenScale);
        halfSize = px * 0.5;

        // 传递 uScale 给 PS，而不是 pointSize（与 Vertex Pulling 一致）
        vScaleFactor = uScale;
        vOpacity = col.a;
        vColor = col.rgb;

        clipPos = MulRows(mvPosition, uProjRow0, uProjRow1, uProjRow2, uProjRow3);
    }

    // 四个角的偏移（屏幕空间）
    float2 corners[4] = {
        float2(-1, -1),
        float2(-1,  1),
        float2( 1,  1),
        float2( 1, -1)
    };

    // 组内索引直接使用线程 ID
    uint baseVert = gtid * VERTS_PER_PARTICLE;
    uint basePrim = gtid * PRIMS_PER_PARTICLE;

    // 生成 4 个顶点
    for (uint i = 0; i < VERTS_PER_PARTICLE; i++)
    {
        VertexOutput v;
        v.UV = corners[i] * 0.5 + 0.5;
        v.vColor = vColor;
        v.vDist = dist;
        v.vOpacity = valid ? vOpacity : 0.0;
        v.vScaleFactor = vScaleFactor;
        v.vIsRing = p.isRing;

        if (valid)
        {
            float2 offset = corners[i] * halfSize;
            v.Pos = clipPos;
            v.Pos.xy += offset * uViewportParams.xy * clipPos.w;
        }
        else
        {
            v.Pos = float4(2, 2, 2, 1);
        }

        verts[baseVert + i] = v;
    }

    // 生成 2 个三角形
    tris[basePrim + 0] = uint3(baseVert + 0, baseVert + 1, baseVert + 2);
    tris[basePrim + 1] = uint3(baseVert + 0, baseVert + 2, baseVert + 3);
}
