#define HLSL_CODE 1
// #define MY_IMPL
cbuffer TexturesIndexBuffer : register(b0)
{
    uint gBlurIndex;
    uint gOutputIndex;
    uint gSceneIndex;
    uint PADDING;
};

[numthreads(8, 8, 1)]
void CSMain(int3 dispatchThreadID : SV_DispatchThreadID) // threads' global ID (within dispatch)
{
    Texture2D gInput = ResourceDescriptorHeap[gBlurIndex]; // SRV
    Texture2D gColor = ResourceDescriptorHeap[gSceneIndex]; // SRV
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gOutputIndex]; // UAV
    uint2 uv = dispatchThreadID.xy;
    
#ifdef MY_IMPL
    // simple implementation, does not care about image borders
    float4 color = gInput[uv] - gInput[uv + uint2(1, 0)]
        + gInput[uv] - gInput[uv + uint2(-1, 0)]
        + gInput[uv] - gInput[uv + uint2(0, 1)]
        + gInput[uv] - gInput[uv + uint2(0, -1)]
        + gInput[uv] - gInput[uv + uint2(1, 1)]
        + gInput[uv] - gInput[uv + uint2(1, -1)]
        + gInput[uv] - gInput[uv + uint2(-1, 1)]
        + gInput[uv] - gInput[uv + uint2(-1, -1)];
    color = 1 - color / 8.0f;
    color = color * gInput[uv];
    color.w = 1.0f;
    gOutput[uv] = color;
#else
    float4 c[3][3];
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            int2 xy = uv + int2(-1 + j, -1 + i);
            c[i][j] = gInput[xy];
        }
    }
    float4 Gx = -1.0f * c[0][0] - 2.0f * c[1][0] - 1.0f * c[2][0] + 1.0f * c[0][2] + 2.0f * c[1][2] + 1.0f * c[2][2];
    float4 Gy = -1.0f * c[2][0] - 2.0f * c[2][1] - 1.0f * c[2][2] + 1.0f * c[0][0] + 2.0f * c[0][1] + 1.0f * c[0][2];
    float4 mag = sqrt(Gx * Gx + Gy * Gy);
    mag = 1.0f - saturate(dot(mag.rgb, float3(0.299f, 0.587f, 0.114f))); // saturate(luminance)
    gOutput[uv] = mag * gColor[uv];
#endif    
}
