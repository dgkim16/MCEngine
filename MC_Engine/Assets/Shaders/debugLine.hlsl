// debugLine.hlsl
// Simple unlit shader for debug line-list visualization (bounding boxes, frustum).
//
// Root signature:
//   param[0]: 4 root constants at b0  (float4 gColor)
//   param[1]: root CBV       at b1  (PerPassCB — only gViewProj is read)

cbuffer cbDebugColor : register(b0)
{
    float4 gColor;
}

// Only the fields we need from PerPassCB — layout must match SharedTypes.h up to gViewProj.
cbuffer PerPassPartial : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
};

float4 VS(float3 posW : POSITION) : SV_POSITION
{
    return mul(float4(posW, 1.0f), gViewProj);
}

float4 PS() : SV_Target
{
    return gColor;
}
