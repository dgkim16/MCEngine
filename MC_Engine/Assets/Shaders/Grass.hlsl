#define HLSL_CODE 1
#include "SharedTypes.h"
#include "lighting.hlsl"
#include "hlslUtilities.hlsl"

StructuredBuffer<MaterialData> gMaterialData : register(t0);
StructuredBuffer<GrassInstanceData> gInstanceData : register(t1);
SamplerState anisotropicWrap : register(s4);

// combind tessellation + instancing

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct HullIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    uint InstID : INSTANCEID;
};

HullIn VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    HullIn vout;
    vout.PosL = vin.PosL;
    vout.NormalL = vin.NormalL;
    vout.TexC = vin.TexC;
    vout.InstID = instanceID; // forward to HS
    return vout;
}

// ── HS patch-constant ───────────────────────────────────────────────
struct PatchTess
{
    float EdgeTess[4] : SV_TessFactor;
    float InsideTess[2] : SV_InsideTessFactor;
};

PatchTess GrassDistanceHS(InputPatch<HullIn, 4> patch, uint patchID : SV_PrimitiveID)
{
    // float4x4 world = gInstanceData[patch[0].InstID].World;
    float3 centerL = 0.25f * (patch[0].PosL + patch[1].PosL + patch[2].PosL + patch[3].PosL);
    // float3 centerW = mul(float4(centerL, 1.0f), world).xyz;
    float3 centerW = centerL;
    float d = distance(centerW, gEyePosW);

    const float d0 = 10.0f, d1 = 30.0f, maxTess = 3.0f;
    float tess = max(1.0f, maxTess * saturate((d1 - d) / (d1 - d0)));
    
    tess = 1.0f; // disables tessellation - tessellator is skipped

    PatchTess pt;
      // Grass is thin — only tessellate the vertical edges (1 and 3)
    pt.EdgeTess[0] = 1; // bottom horizontal edge
    pt.EdgeTess[1] = tess; // left vertical edge
    pt.EdgeTess[2] = 1; // top horizontal edge
    pt.EdgeTess[3] = tess; // right vertical edge
    pt.InsideTess[0] = 1; // horizontal interior
    pt.InsideTess[1] = tess; // vertical interior
    return pt;
}

  // ── HS control-point ────────────────────────────────────────────────
struct HullOut
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    uint InstID : INSTANCEID;
};

[domain("quad")] // always outputs at least 2 triangles
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("GrassDistanceHS")]
[maxtessfactor(8.0f)]
  HullOut HS(InputPatch<HullIn, 4> p, uint i : SV_OutputControlPointID)
{
    HullOut o;
    o.PosL = p[i].PosL;
    o.NormalL = p[i].NormalL;
    o.TexC = p[i].TexC;
    o.InstID = p[i].InstID;
    return o;
}

  // ── DS ─────────────────────────────────────────────────────────────
struct DomainOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    nointerpolation uint MatIndex : MATINDEX;
    // float BendF : VALUE;   // interpolated
};

[domain("quad")]
DomainOut DS(PatchTess pt, float2 uv : SV_DomainLocation, const OutputPatch<HullOut, 4> quad)
{
    // bilinear interpolation over the 4 control points
    float3 p = lerp(lerp(quad[0].PosL, quad[1].PosL, uv.x), lerp(quad[2].PosL, quad[3].PosL, uv.x), uv.y);
    float3 n = lerp(lerp(quad[0].NormalL, quad[1].NormalL, uv.x), lerp(quad[2].NormalL, quad[3].NormalL, uv.x), uv.y);
    float2 tc = lerp(lerp(quad[0].TexC, quad[1].TexC, uv.x), lerp(quad[2].TexC, quad[3].TexC, uv.x), uv.y);
    
    GrassInstanceData inst = gInstanceData[quad[0].InstID];
    float c = inst.cosYaw;
    float s = inst.sinYaw;
    float scale = inst.scale;
    float4 posWorld;
    float3 normW;
    posWorld.x = scale * (p.x * c - p.z * s) + inst.grassPosition.x;
    normW.x = (n.x * c - n.z * s);
    posWorld.y = scale * p.y;
    normW.y = n.y;
    posWorld.z = scale * (p.x * s + p.z * c) + inst.grassPosition.z;
    normW.z = (n.x * s + n.z * c);
    posWorld.w = 1.0f;
    normW = normalize(normW);
    float4 posW = posWorld;
    
    // WIND
    
    static const float3 windDir = float3(-1.0f, 0.0f, 0.0f); // world-space
    static const float windStrength = 0.6f;
    static const float noiseFreq = 0.25f;
    static const float windSpeed = 0.5f;
    float2 noisePos = float2(posW.x * noiseFreq + gTotalTime * windSpeed, posW.z * noiseFreq);
    float n1 = PerlinNoise2D(noisePos.x, noisePos.y); // large gust  [-1,1]
    float n2 = PerlinNoise2D(noisePos.x * 2.7f + 5.3f, noisePos.y * 2.7f); // turbulence  [-1,1]
    float noise = n1 * 0.7f + n2 * 0.3f; // FBM blend
    float bendF = saturate(noise * 0.5f + 0.5f); // remap to [0,1]
    float WindBendFactor = uv.y * uv.y * bendF;
    
    // posW.xyz += windDir * windStrength * WindBendFactor;
    
    // imaginary sphere moving around in circle
    float r = 4.0f;
    float tR = 10.0f;
    float rSpd = 0.5f;
    float3 rotPos = float3(tR * sin(gTotalTime * rSpd), r, tR * cos(gTotalTime * rSpd));
    float3 dir = posW.xyz - rotPos;
    //dir.y = 0.0f;
    float mag = length(dir);
    float lerpf = min(1, mag / r);
    float3 bendDir = lerp(dir, windDir, lerpf * lerpf);
    float bendStrength = lerp(2.0f, windStrength, lerpf * lerpf);
    float bendFactor = lerp(uv.y * uv.y * lerp(0.9f, 0.0f, lerpf), WindBendFactor, lerpf * lerpf);
    posW.xyz += bendDir * bendStrength * bendFactor;
    
    
    DomainOut dout;
    dout.PosH = mul(posW, gViewProj);
    dout.PosW = posW.xyz;
    //dout.NormalW = mul(n, (float3x3) World);
    dout.TexC = tc;
    dout.NormalW = normW;

    dout.MatIndex = (uint)gMatIndex;
    
    // dout.BendF = bendFactor;
    return dout;
}

// ── PS ─────────────────────────────────────────────────────────────────────
// All lighting uses an upward normal (0,1,0) so every blade receives identical
// illumination regardless of its random Y rotation. The geometric normal from
// the DS is rotation-dependent, which makes Kd (via Fresnel) and Intensity
// both vary per blade — causing some blades to appear dark.
float4 PS(DomainOut pin) : SV_Target
{
    MaterialData matData = gMaterialData[gMatIndex]; // use b0 cbuffer's gMatIndex for all instances
    float4 diffuseAlbedo = matData.DiffuseAlbedo;
#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif
    // wind vector displayed as color on grass
    // diffuseAlbedo *= pin.BendF;
    
    // Root darkening: TexC.y = 1 at root, 0 at tip → lerp dark→full as TexC.y decreases
    diffuseAlbedo.rgb *= lerp(1.0f, 0.3f, pin.TexC.y);

    // Sky-facing normal: uniform across all blades regardless of Y rotation
    float3 lightingN = float3(0.0f, 1.0f, 0.0f);
    lightingN = normalize(lightingN);
    float3 toEyeW = normalize(gEyePosW - pin.PosW);
    float3 lightVec = normalize(float3(1.0f, 1.0f, 1.0f));

    /*
    float Intensity = saturate(dot(lightVec, lightingN)) * 3.0f;
    float shininess = 1.0f - matData.Roughness;
    float m = shininess * 256.0f;
    float3 halfvec = normalize(toEyeW + lightVec);
    float roughnessFactor = (m + 8.0f) / 8.0f * pow(max(dot(halfvec, lightingN), 0.0f), m);
    float3 fresnelFactor = SchlickFresnel(matData.FresnelR0, lightingN, lightVec);
    float3 lambert = diffuseAlbedo.rgb / PI;
    float3 Kd = (float3(1.0f, 1.0f, 1.0f) - fresnelFactor) * lambert;
    float3 specAlbedo = fresnelFactor * roughnessFactor;
    specAlbedo = specAlbedo / (specAlbedo + 1.0f);
    float4 ambient = gAmbientLight * diffuseAlbedo;
    */
    // float4 litColor = ambient + saturate(float4((Kd + specAlbedo) * Intensity, 1.0f));
    float3 fresnelr0 = matData.FresnelR0;
    float roughness = matData.Roughness;
    float4 litColor = SimpleBlinnPhong(diffuseAlbedo, gAmbientLight, fresnelr0, lightingN, toEyeW, pin.PosW, 1.0 - roughness, gLights[0]);
    // Fog
    float distToEye = length(gEyePosW - pin.PosW);
    float fogAmount = saturate((distToEye - gFogStart) / gFogRange);
    litColor = lerp(litColor, gFogColor, fogAmount);

    litColor.a = 1.0f;
    return litColor;
}