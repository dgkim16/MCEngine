#define HLSL 1
Texture2D gNormalMap : register(t0);
SamplerState gsamLinear : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    int gMatIndex;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1; // pad to align by 16 bytes
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;

    float4 gFogColor;
    float gFogStart;
    float gFogRange;
    float2 cbPerObjectPad2;
};

cbuffer matPass0 : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
}

cbuffer tellPass : register(b3)
{
    int gOuterTessFactor = 1;
    int gInnerTessFactor = 1;
}

#ifdef DRAW_INSTANCED
struct InstanceData
{
    float4x4 World;
    float4x4 TexTransform;
    uint MaterialIndex;
};
StructuredBuffer<InstanceData> gInstanceData : register(t1);
#endif

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
#ifdef DRAW_INSTANCED
    uint InstID : INSTANCEID;
#endif
};

VertexOut VS(VertexIn vin
#ifdef DRAW_INSTANCED
    , uint instanceID : SV_InstanceID
#endif
    )
{
    VertexOut vout;
    vout.PosL = vin.PosL;
    vout.TexC = vin.TexC;
    vout.NormalL = vin.NormalL;
#ifdef DRAW_INSTANCED
    vout.InstID = instanceID;
#endif
    return vout;
}

struct PatchTess // Quad
{
    float EdgeTess[4] : SV_TessFactor;
    float InsideTess[2] : SV_InsideTessFactor;
};

// Distance based HS
PatchTess DistanceHS(InputPatch<VertexOut, 4> patch,
    uint patchID : SV_PrimitiveID)
{
    int OuterTessFactor = 1;
    int InnerTessFactor = 1;

    PatchTess pt;
    float3 centerL = 0.25f * (patch[0].PosL + patch[1].PosL + patch[2].PosL + patch[3].PosL);
#ifdef DRAW_INSTANCED
    float3 centerW = mul(float4(centerL, 1.0f), gInstanceData[patch[0].InstID].World).xyz;
#else
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;
#endif
    float d = distance(centerW, gEyePosW);
    const float d0 = 5.0f;
    const float d1 = 15.0f;
    float tess = max(1.0f, 32.0f * saturate((d1 - d) / (d1 - d0)));

    pt.EdgeTess[0] = tess * OuterTessFactor;
    pt.EdgeTess[1] = tess * OuterTessFactor;
    pt.EdgeTess[2] = tess * OuterTessFactor;
    pt.EdgeTess[3] = tess * OuterTessFactor;

    pt.InsideTess[0] = tess * InnerTessFactor;
    pt.InsideTess[1] = tess * InnerTessFactor;
    return pt;
}

// Constant HS
PatchTess ConstantHS(InputPatch<VertexOut, 4> patch,
    uint patchID : SV_PrimitiveID)
{
    int OuterTessFactor = 1;
    int InnerTessFactor = 1;
    // uniformly tessellate patch 3 times
    PatchTess pt;
    pt.EdgeTess[0] = 1 * OuterTessFactor;
    pt.EdgeTess[1] = 1 * OuterTessFactor;
    pt.EdgeTess[2] = 1 * OuterTessFactor;
    pt.EdgeTess[3] = 1 * OuterTessFactor;
    
    pt.InsideTess[0] = 1 * InnerTessFactor;
    pt.InsideTess[1] = 1 * InnerTessFactor;
    return pt;
}

struct HullOut
{
    float3 PosL : POSITION;
    float2 TexC : TEXCOORD;
    float3 NormalL : NORMAL;
#ifdef DRAW_INSTANCED
    uint InstID : INSTANCEID;
#endif
};

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("DistanceHS")]
[maxtessfactor(32.0f)]
HullOut HS(InputPatch<VertexOut, 4> p, uint i : SV_OutputControlPointID, uint patchId : SV_PrimitiveID)
{
    HullOut hout;
    hout.PosL = p[i].PosL;
    hout.TexC = p[i].TexC;
    hout.NormalL = p[i].NormalL;
#ifdef DRAW_INSTANCED
    hout.InstID = p[i].InstID;
#endif
    return hout;
}


struct DomainOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
#ifdef DRAW_INSTANCED
    nointerpolation uint MatIndex : MATINDEX;
#endif
};

[domain("quad")]
DomainOut DS(PatchTess patchTess,
             float2 uv : SV_DomainLocation,
             const OutputPatch<HullOut, 4> quad)
{
    DomainOut dout;

    float3 v1 = lerp(quad[0].PosL, quad[1].PosL, uv.x);
    float3 v2 = lerp(quad[2].PosL, quad[3].PosL, uv.x);
    float3 p = lerp(v1, v2, uv.y);

    float3 n1 = lerp(quad[0].NormalL, quad[1].NormalL, uv.x);
    float3 n2 = lerp(quad[2].NormalL, quad[3].NormalL, uv.x);
    float3 n = lerp(n1, n2, uv.y);

    float2 t1 = lerp(quad[0].TexC, quad[1].TexC, uv.x);
    float2 t2 = lerp(quad[2].TexC, quad[3].TexC, uv.x);
    float2 texc = lerp(t1, t2, uv.y);

#ifdef DRAW_INSTANCED
    InstanceData inst = gInstanceData[quad[0].InstID];
    float4 posW = mul(float4(p, 1.0f), inst.World);
    dout.NormalW = mul(n, (float3x3) inst.World);
    dout.MatIndex = inst.MaterialIndex;
#else
    p.z = 0.3f * (p.y * sin(p.x) + p.x * cos(p.y));
    float4 posW = mul(float4(p, 1.0f), gWorld);
    dout.NormalW = mul(n, (float3x3) gWorld);
#endif

    dout.TexC = texc;
    dout.PosW = posW.xyz;
    dout.PosH = mul(posW, gViewProj);
    return dout;
}