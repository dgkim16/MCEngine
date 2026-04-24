
#define HLSL_CODE 1
static const int gMaxBlurRadius = 15;

#define N 256
#define CacheSize (N + 2*gMaxBlurRadius)
groupshared float4 gCache[CacheSize];

cbuffer BlurDispatchCB : register(b0)
{
    float4 gWeightVec[8]; // 8*4=32 floats for max blur radius of 15.
    // float4 is more register friendly, but introduces indexing overhead, thus user unfriendly
    // float[32] is more user friendly, but less convenient for CPU side uplaoding

    int gBlurRadius;
    uint gBlurInputIndex;
    uint gBlurOutputIndex;
    uint BlurDispatchCB_Pad0;
};

// float GaussianWeights[5] = (0.0545f, 0.2442f, 0.4026f, 0.2422f, 0.0545f);

[numthreads (N,1,1)]
void HorizontalBlurCS(int3 groupThreadID : SV_GroupThreadID, // thread's local ID (within group)
        int3 dispatchThreadID : SV_DispatchThreadID) // threads' global ID (within dispatch)
{
    Texture2D gInput = ResourceDescriptorHeap[gBlurInputIndex]; // SRV
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gBlurOutputIndex]; // UAV
    uint2 imgDims;
    gInput.GetDimensions(imgDims.x, imgDims.y);
    // store values of Input image into cache (extra 2R space for blurring)
    // clamp image's edge values to prevent black borders (if not clamped, they become 0)
    if (groupThreadID.x < gBlurRadius)
    {
        int x = max((int) dispatchThreadID.x - gBlurRadius, 0);
        gCache[groupThreadID.x] = gInput[uint2(x, dispatchThreadID.y)];
    }
    if (groupThreadID.x >= N - gBlurRadius)
    {
        int x = min(dispatchThreadID.x + gBlurRadius, imgDims.x - 1);
        gCache[groupThreadID.x + 2 * gBlurRadius] = gInput[uint2(x, dispatchThreadID.y)];
    }
    gCache[groupThreadID.x + gBlurRadius] = gInput[min(dispatchThreadID.xy, imgDims - 1)];
    
    GroupMemoryBarrierWithGroupSync(); // sync, aka waits until all threads in this group reaches this point
    
    float4 blurColor = float4(0, 0, 0, 0);
    for (int i = -gBlurRadius; i <= gBlurRadius; ++i)
    {
        int k = groupThreadID.x + gBlurRadius + i;
        
        int float4Index = (i + gBlurRadius) / 4;
        int slotIndex = (i + gBlurRadius) & 0x3;
        // 0x3 = 011, so (A & 0x3) only keeps the last 2 bits of A
        // which is identical to doing (A % 4) 
        
        float weight = gWeightVec[float4Index][slotIndex];
        blurColor += weight * gCache[k];
    }
    
    gOutput[dispatchThreadID.xy] = blurColor;
    gOutput[dispatchThreadID.xy].w = 1.0f;

}

[numthreads(1, N, 1)]
void VerticalBlurCS(int3 groupThreadID : SV_GroupThreadID, // thread's local ID (within group)
        int3 dispatchThreadID : SV_DispatchThreadID) // threads' global ID (within dispatch)
{
    Texture2D gInput = ResourceDescriptorHeap[gBlurInputIndex]; // SRV
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gBlurOutputIndex]; // UAV
    uint2 imgDims;
    gInput.GetDimensions(imgDims.x, imgDims.y);
    // store values of Input image into cache (extra 2R space for blurring)
    // clamp image's edge values to prevent black borders (if not clamped, they become 0)
    if (groupThreadID.y < gBlurRadius)
    {
        int y = max((int) dispatchThreadID.y - gBlurRadius, 0);
        gCache[groupThreadID.y] = gInput[uint2(dispatchThreadID.x, y)];
    }
    if (groupThreadID.y >= N - gBlurRadius)
    {
        int y = min(dispatchThreadID.y + gBlurRadius, imgDims.y - 1);
        gCache[groupThreadID.y + 2 * gBlurRadius] = gInput[uint2(dispatchThreadID.x,y)];
    }
    gCache[groupThreadID.y + gBlurRadius] = gInput[min(dispatchThreadID.xy, imgDims - 1)];
    
    GroupMemoryBarrierWithGroupSync(); // sync, aka waits until all threads in this group reaches this point
    
    float4 blurColor = float4(0, 0, 0, 0);
    for (int i = -gBlurRadius; i <= gBlurRadius; ++i)
    {
        int k = groupThreadID.y + gBlurRadius + i;
        
        int float4Index = (i + gBlurRadius) / 4;
        int slotIndex = (i + gBlurRadius) & 0x3;
        // 0x3 = 011, so (A & 0x3) only keeps the last 2 bits of A
        // which is identical to doing (A % 4) 
        
        float weight = gWeightVec[float4Index][slotIndex];
        blurColor += weight * gCache[k];
    }
    
    gOutput[dispatchThreadID.xy] = blurColor;
    gOutput[dispatchThreadID.xy].w = 1.0f;
}
