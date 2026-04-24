#ifdef DEPTH_MSAA
Texture2DMS<float> gDepthTex : register(t0);
#else
Texture2D gDepthTex : register(t0);
SamplerState gsamPointClamp : register(s0);
#endif

cbuffer cbDebug : register(b0)
{
    float gNearZ;
    float gFarZ;
    float gVisualMaxDepth;
    float gPad0;
};

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

float LinearizeDepth(float z, float n, float f)
{
    return (n * f) / (f - z * (f - n));
}


VSOut VS(uint vid : SV_VertexID)
{
    VSOut vout;

    float2 pos[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    float2 uv[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    vout.PosH = float4(pos[vid], 0.0f, 1.0f);
    vout.TexC = uv[vid];
    return vout;
}

float4 PS(VSOut pin) : SV_Target
{
#ifdef DEPTH_MSAA
    int2 p = int2(pin.PosH.xy);
    float d = (gDepthTex.Load(p, 0) + gDepthTex.Load(p, 1) + gDepthTex.Load(p, 2) + gDepthTex.Load(p, 3)) * 0.25f;
#else
    float d = gDepthTex.Sample(gsamPointClamp, pin.TexC).r;
#endif
    float linearDepth = LinearizeDepth(d, gNearZ, gFarZ);
    float v = saturate(linearDepth / gVisualMaxDepth);
    return float4(v, v, v, 1.0f);
}