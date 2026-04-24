#define HLSL_CODE 1

cbuffer TexturesIndexBuffer : register(b0)
{
    uint gInputIndex; 
    uint gOutputIndex;
    uint PADDING0;
    uint PADDING1;
};

[numthreads(16, 16, 1)]
void CSMain(int3 dispatchThreadID : SV_DispatchThreadID) // threads' global ID (within dispatch)
{
    Texture2D gInput = ResourceDescriptorHeap[gInputIndex]; // SRV
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gOutputIndex]; // UAV
    
    uint2 uv = dispatchThreadID.xy;
    float4 color = gInput[uv];
    color.a = 1.0f;
    gOutput[uv] = color;
}
