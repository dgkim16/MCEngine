#define HLSL_CODE 1
#include "SharedTypes.h"
#include "lighting.hlsl"

StructuredBuffer<MaterialData> gMaterialData : register(t0);
StructuredBuffer<InstanceData> gInstanceData : register(t1);

SamplerState gsamLinear : register(s0); // see d3dUtil::GetStaticSamplers
SamplerState anisotropicWrap : register(s4);

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
    float4x4 world = gWorld;
    float4x4 texTransform = gTexTransform;
    
#endif
    MaterialData matData = gMaterialData[gMatIndex];
    float4 posW = mul(float4(vin.PosL, 1.0f), world);
    vout.PosW = posW.xyz;
    vout.PosH = mul(posW, gViewProj);
	// Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3) world);
	
    float4 texc = mul(float4(vin.TexC, 0.0f, 1.0f), texTransform);
    vout.TexC = mul(texc, matData.matTransform).xy;
    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
// #if DRAW_INSTANCED
//    MaterialData matData = gMaterialData[pin.MatIndex];
// #else
    MaterialData matData = gMaterialData[gMatIndex];
// #endif
    Texture2D diffuseMap = ResourceDescriptorHeap[matData.DiffuseMapIndex]; //ResourceDescriptorHeap[matData.DiffuseMapIndex];
    float4 diffuseAlbedo = diffuseMap.Sample(anisotropicWrap, pin.TexC) * matData.DiffuseAlbedo;
    float3 fresnelr0 = matData.FresnelR0;
    float roughness = matData.Roughness;
#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif
    // Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);
    float3 normal = pin.NormalW;
    float3 toEyeW = gEyePosW - pin.PosW;
    float distToEye = length(toEyeW);
    toEyeW /= distToEye; // normalize
    float4 litColor = SimpleBlinnPhong(diffuseAlbedo, gAmbientLight, fresnelr0, normal, toEyeW, pin.PosW, 1.0 - roughness, gLights[0]);
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


