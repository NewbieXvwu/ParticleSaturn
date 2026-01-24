struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEX_COORD;
};

VSOut main(uint VertID : SV_VertexID)
{
    float2 pos[4] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };

    float2 uv[4] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    VSOut o;
    o.Pos = float4(pos[VertID], 0.0, 1.0);
    o.UV  = uv[VertID];
    return o;
}
