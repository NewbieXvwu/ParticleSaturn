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

    // x = 2/width, y = 2/height
    float4 uViewportParams;

    // x = timeSeconds
    float4 uTimeParams;
};

struct VSIn
{
    float3 Pos   : ATTRIB0; // world
    float3 Color : ATTRIB1;
    float  Size  : ATTRIB2;
    float  Seed  : ATTRIB3;
};

struct VSOut
{
    float4 Pos   : SV_POSITION;
    float3 Color : COLOR0;
    float  Seed  : TEXCOORD0;
    float2 UV    : TEXCOORD1;
};

float4 MulRows(float4 v, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

VSOut main(uint VertId : SV_VertexID, VSIn i)
{
    // 两个三角形拼一个 quad（以中心为 anchor 的 billboard）
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
    float2 uv     = corner * 0.5 + 0.5;

    float4 worldPos = MulRows(float4(i.Pos, 1.0), uModelRow0, uModelRow1, uModelRow2, uModelRow3);
    float4 viewPos  = MulRows(worldPos,            uViewRow0,  uViewRow1,  uViewRow2,  uViewRow3);
    float4 clipPos  = MulRows(viewPos,             uProjRow0,  uProjRow1,  uProjRow2,  uProjRow3);

    // OpenGL 旧逻辑假设：相机前方 viewPos.z < 0，使用 -viewPos.z 做深度缩放。
    // 为了避免"在相机后方的点"因为 clamp 导致尺寸异常，直接裁剪掉。
    if (viewPos.z >= -0.001)
    {
        VSOut o;
        o.Pos   = float4(2.0, 2.0, 2.0, 1.0); // 保证被裁剪
        o.Color = float3(0.0, 0.0, 0.0);
        o.Seed  = 0.0;
        o.UV    = float2(0.0, 0.0);
        return o;
    }

    float invZ = 1.0 / (-viewPos.z);
    float px   = clamp(i.Size * (1000.0 * invZ), 1.0, 8.0);

    // 从像素尺寸转换到 NDC 偏移，再转回 clip space（保持屏幕空间恒定大小）。
    float2 ndcCenter = clipPos.xy / clipPos.w;
    float2 ndcOffset = corner * (px * 0.5) * uViewportParams.xy;
    float2 ndcPos    = ndcCenter + ndcOffset;

    VSOut o;
    o.Pos   = float4(ndcPos * clipPos.w, clipPos.z, clipPos.w);
    o.Color = i.Color;
    o.Seed  = i.Seed;
    o.UV    = uv;
    return o;
}
