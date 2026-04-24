// HLSL_CODE must be set before including SharedTypes.h so that:
//   - DEFINE_CBUFFER(Name, Reg) expands to: cbuffer Name : register(Reg)
//   - type aliases (float4x4, float3, uint, ...) are native HLSL types
#define HLSL_CODE 1
#include "SharedTypes.h"

// GrassCullCB is declared by DEFINE_CBUFFER(GrassCullCB, b0) in SharedTypes.h,
// which expands to:  cbuffer GrassCullCB : register(b0) { ... }
// Fields available: FrustumPlanes, EyePosW, DrawDistance, InstanceCount, GrassMaterialIndex, SphereRadius

StructuredBuffer<GrassInstanceData> gAllInstances : register(t0); // mGrassFullInstanceBuffer
RWStructuredBuffer<GrassInstanceData> gVisible : register(u0); // mGrassVisibleBuffer
globallycoherent RWByteAddressBuffer gVisibleCount : register(u1); // mGrassCounterBuffer

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= InstanceCount)
        return;

    GrassInstanceData inst = gAllInstances[id.x];
    // float3 centerW = float3(inst.World._41, inst.World._42, inst.World._43);
    float3 centerW = inst.grassPosition;

    // Distance cull (against base position)
    if (distance(centerW, EyePosW) >= DrawDistance)
        return;

    // World-space sphere-frustum cull.
    // Bounding sphere: center = blade base (centerW), radius = SphereRadius (= grassHeight).
    // FrustumPlanes[i] are normalized; dot(plane.xyz, P) + plane.w >= 0 means P is inside.
    // If the sphere is fully outside any plane, cull the blade.
    for (int i = 0; i < 6; i++)
    {
        if (dot(FrustumPlanes[i].xyz, centerW) + FrustumPlanes[i].w < -SphereRadius)
            return;
    }

    uint slot;
    gVisibleCount.InterlockedAdd(0, 1, slot);
    // Override MaterialIndex with the value supplied by GrassCullCB at dispatch time.
    // The static instance buffer was uploaded before MCEngine reassigns MatCBIndex, so
    // inst.MaterialIndex may be stale; GrassMaterialIndex is always current.
    // inst.MaterialIndex = GrassMaterialIndex;
    gVisible[slot] = inst; // this is contiguous in memory
}