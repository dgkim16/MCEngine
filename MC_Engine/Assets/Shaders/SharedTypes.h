// Put code here that is shared between HLSL code and C++ code.
//
// We also employ macros so that we can essentially share the
// same code between HLSL and C++ instead of having two versions.

#if HLSL_CODE

#define DEFINE_CBUFFER(Name, Reg) cbuffer Name : register(Reg)
#ifndef SHARED_TYPES
#define SHARED_TYPES
#endif 
#elif __cplusplus

#pragma once

#include <cstdint>
#include <DirectXMath.h>

#define DEFINE_CBUFFER(Name, Reg) struct Name

#define uint uint32_t
#define uint2 DirectX::XMUINT2
#define uint3 DirectX::XMUINT3
#define uint4 DirectX::XMUINT4

#define int2 DirectX::XMINT2
#define int3 DirectX::XMINT3
#define int4 DirectX::XMINT4

#define float2 DirectX::XMFLOAT2
#define float3 DirectX::XMFLOAT3
#define float4 DirectX::XMFLOAT4
#define float4x4 DirectX::XMFLOAT4X4

// ── C++-only GPU upload structs ──────────────────────────────────────────────
// These are filled on the CPU and uploaded to GPU constant/structured buffers.
// Their memory layouts must match the corresponding HLSL cbuffer/struct.

struct MaterialConstants      // mirrors MaterialData (StructuredBuffer t0)
{
    float4   DiffuseAlbedo;
    float3   FresnelR0;
    float    Roughness;
    float4x4 MatTransform;
    int      srvIndex;
};

struct DebugDepthConstants    // mirrors cbDebug (b0) in depthDebug.hlsl
{
    float NearZ;
    float FarZ;
    float VisualMaxDepth;
    float Pad0;
};

struct CSB_default            // mirrors TexturesIndexBuffer (b0) in ForceAlpha/SobelCS
{
    int InputIndex;
    int OutputIndex;
    int Width;
    int Height;
};

struct CSB_blur               // mirrors BlurDispatchCB (b0) in blursCS.hlsl
{
    float4 WeightVec[8];
    int    BlurRadius;
    int    InputIndex;
    int    OutputIndex;
};

#endif

// Fixed indices in sampler heap.
#define SAM_POINT_WRAP 0
#define SAM_POINT_CLAMP 1
#define SAM_LINEAR_WRAP 2
#define SAM_LINEAR_CLAMP 3
#define SAM_ANISO_WRAP 4
#define SAM_ANISO_CLAMP 5
#define SAM_SHADOW 6

#define MAX_LIGHTS 1

DEFINE_CBUFFER(PerObjectCB, b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    int gMatIndex;
};

struct LightData
{
    float3 mLightColor;
    float mLightIntensity;
    float3 mLightDirection;     // spotlight, directional light
    float mAttenuationDist;     // punctual lights only (spot & point)
    float mLightRadius;         // punctual lights only (spot & point)
    float3 mLightPosition;      // punctual lights only (spot & point)
    int mLightType;             // 0 directional, 1 point, 2 spot, 3 count
};

DEFINE_CBUFFER(PerPassCB, b1)   // allowed upto 64KB per constant buffer (CBV)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;      // pad to align by 16 bytes
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

    LightData gLights[MAX_LIGHTS]; // forward rendering approach
};

// Structured Buffer
struct MaterialData
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Roughness;
    float4x4 matTransform;

    uint DiffuseMapIndex;
};

struct InstanceData         // large vertex per instance, small instance count
{
    float4x4 World;        // was gWorld
    float4x4 TexTransform; // was gTexTransform
    // uint MaterialIndex;    // was gMatIndex
};

struct GrassInstanceData // small vertex per instance, large instance count
{
    // float4x4 World;
    float3 grassPosition;
    float sinYaw;
    float cosYaw;
    float scale;
};

DEFINE_CBUFFER(GrassCullCB, b0)
{
    float4   FrustumPlanes[6];    // world-space normalized planes; dot(p.xyz,C)+p.w >= 0 = inside
    float3   EyePosW;
    float    DrawDistance;
    uint     InstanceCount;
    uint     GrassMaterialIndex;  // set at dispatch time (correct after MatCBIndex reassignment)
    float    SphereRadius;        // blade bounding sphere radius = grassHeight
    uint     _pad;
};