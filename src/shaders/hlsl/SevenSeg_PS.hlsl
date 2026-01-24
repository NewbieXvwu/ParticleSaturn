struct PSIn
{
    float4 Pos : SV_POSITION;
    float3 Col : COLOR;
};

float4 main(PSIn i) : SV_Target
{
    return float4(i.Col, 1.0);
}
