#define HLSL_CODE 1
#include "SharedTypes.h"
#include "lighting.hlsl"

StructuredBuffer<MaterialData> gMaterialData : register(t0);
StructuredBuffer<InstanceData> gInstanceData : register(t1);
StructuredBuffer<InstanceData> gObjectData : register(t2);
StructuredBuffer<LightData> gLightData : register(t3);

SamplerState gsamLinear : register(s0); // see d3dUtil::GetStaticSamplers
SamplerState anisotropicWrap : register(s4);
SamplerComparisonState gsamShadow : register(s6);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float4 ShadowPosH : SHADOWPOS;
    float3 LightDir : LIGHTDIR;
#ifdef DRAW_INSTANCED
    nointerpolation uint MatIndex : MATINDEX;   // Do NOT interpolate. Just use the value from one vertex for the whole primitive. otherwise value gets interpolated for each Pixel between vertices of triangle
#endif
};

VertexOut VS(VertexIn vin
#ifdef DRAW_INSTANCED
    , uint instanceID : SV_InstanceID
#endif
    )
{
    VertexOut vout = (VertexOut) 0.0f;
#if DRAW_INSTANCED
    InstanceData instData = gInstanceData[instanceID];
    float4x4 world = instData.World;
    float4x4 texTransform = instData.TexTransform;
    // uint matIndex = instData.MaterialIndex;
    // vout.MatIndex = matIndex;
    // MaterialData matData = gMaterialData[matIndex];
#else
	// Transform to homogeneous clip space.
    float4x4 world = gObjectData[gObjIndex].World;
    float4x4 texTransform = gObjectData[gObjIndex].TexTransform;
    
#endif
    MaterialData matData = gMaterialData[gObjectData[gObjIndex].MaterialIndex];
    float4 posW = mul(float4(vin.PosL, 1.0f), world);
    vout.PosW = posW.xyz;
    vout.PosH = mul(posW, gViewProj);
	// Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3) world);
    vout.ShadowPosH = mul(posW, gLightData[0].mShadowTransform);
    vout.LightDir = (gLightData[0].mLightType == 0) ? (gLightData[0].mLightDirection) : (gLightData[0].mLightPosition - posW.xyz);
    float4 texc = mul(float4(vin.TexC, 0.0f, 1.0f), texTransform);
    vout.TexC = mul(texc, matData.matTransform).xy;
    
    return vout;
}

float CalcShadowFactor(float4 shadowPosH, uint smapIdx, float bias)
{
    shadowPosH.xyz /= shadowPosH.w; // no-op for ortho (w=1); keeps spot-ready
    Texture2D smap = ResourceDescriptorHeap[smapIdx];
    return max(smap.SampleCmpLevelZero(gsamShadow, shadowPosH.xy, shadowPosH.z), 0.4f); // this does Percentage Bilinear Filtering
    // return max(smap.SampleCmpLevelZero(gsamShadow, shadowPosH.xy, shadowPosH.z - bias), 0.4f); // this does Percentage Bilinear Filtering
}

float4 PS(VertexOut pin) : SV_Target
{
// #if DRAW_INSTANCED
//    MaterialData matData = gMaterialData[pin.MatIndex];
// #else
    MaterialData matData = gMaterialData[gObjectData[gObjIndex].MaterialIndex];
// #endif
    Texture2D diffuseMap = ResourceDescriptorHeap[matData.DiffuseMapIndex]; //ResourceDescriptorHeap[matData.DiffuseMapIndex];
    float4 diffuseAlbedo = diffuseMap.Sample(anisotropicWrap, pin.TexC) * matData.DiffuseAlbedo;
    float3 fresnelr0 = matData.FresnelR0;
    float roughness = matData.Roughness;
#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif
    // Interpolating normal can unnormalize it, so renormalize it.
    // pin.NormalW = normalize(pin.NormalW);
    float3 normal = normalize(pin.NormalW);
    float3 toEyeW = gEyePosW - pin.PosW;
    float distToEye = length(toEyeW);
    toEyeW /= distToEye; // normalize
    float4 litColor = gAmbientLight.a < 0.25f
                    ? classicBlinnPhong(diffuseAlbedo, gAmbientLight, fresnelr0, normal, toEyeW, pin.PosW, 1.0 - roughness, gLightData[0], pin.LightDir)
                    : gAmbientLight.a < 0.5f
                    ? LunaPhongBRDF(diffuseAlbedo, gAmbientLight, fresnelr0, normal, toEyeW, pin.PosW, 1.0 - roughness, gLightData[0], pin.LightDir)
                    : PhongBRDF(diffuseAlbedo, gAmbientLight, fresnelr0, normal, toEyeW, pin.PosW, 1.0 - roughness, gLightData[0], pin.LightDir);
    if (gLightData[0].mShadowMapIndex != -1)
    {
        // past the pseudo far plane the shadow map holds nothing for this receiver: lit, not shadowed
        bool inShadowRange = mul(float4(pin.PosW, 1.0f), gView).z <= gLightData[0].mShadowFar;
        float shadowFactor = (gLightData[0].mCastShadow && inShadowRange) ? CalcShadowFactor(pin.ShadowPosH, gLightData[0].mShadowMapIndex, gLightData[0].depthBias) : 1.0f;
        // shadow attenuates direct light only; ambient term matches the one each lighting function adds
        float3 ambient = (gAmbientLight.a >= 0.25f && gAmbientLight.a < 0.5f) // LunaPhongBRDF tints ambient by the light color
                       ? gAmbientLight.rgb * diffuseAlbedo.rgb * gLightData[0].mLightColor
                       : gAmbientLight.rgb * diffuseAlbedo.rgb;
        litColor.rgb = ambient + (litColor.rgb - ambient) * shadowFactor;
    }
    
#ifdef FOG
	float fogAmount = saturate((distToEye - gFogStart) / gFogRange);
	litColor = lerp(litColor, gFogColor, fogAmount);
#endif
#ifdef FORCE_OPAQUE_ALPHA
    litColor.a = 1.0f;
#else
    litColor.a = diffuseAlbedo.a;
#endif
    return litColor;
}


