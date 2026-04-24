#define HLSL_CODE 1
#include "SharedTypes.h"
#include "lighting.hlsl"
#include "hlslUtilities.hlsl"

// Texture2DArray gTreeMapArray : register(t0);
StructuredBuffer<MaterialData> gMaterialData : register(t0);
SamplerState gsamLinear : register(s0); // see d3dUtil::GetStaticSamplers
SamplerState gsamPointClamp : register(s1);
SamplerState gsamLinearWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);
SamplerState gsamAnisotropicWrap : register(s4);
SamplerState gsamSnisotropicClamp : register(s5);

struct VertexIn
{
    float3 PosW : POSITION;
    float2 SizeW : SIZE;
};

struct VertexOut
{
    float3 CenterW : POSITION;
    float2 SizeW : SIZE;
};

struct GeoOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    uint PrimID : SV_PrimitiveID;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

	// Just pass data over to geometry shader.
    vout.CenterW = vin.PosW;
    vout.SizeW = vin.SizeW;

    return vout;
}


 // We expand each point into a quad (4 vertices), so the maximum number of vertices
 // we output per geometry shader invocation is 4.
[maxvertexcount(4)]
void GS(point VertexOut gin[1],
        uint primID : SV_PrimitiveID,
        inout TriangleStream<GeoOut> triStream)
{
	//
	// Compute the local coordinate system of the sprite relative to the world
	// space such that the billboard is aligned with the y-axis and faces the eye.
	//

    
    float3 up = float3(0.0f, 1.0f, 0.0f); 
    //up = CameraUp(gEyePosW,gin[0].CenterW);  // for UI that always faces camera
    float3 look = gEyePosW - gin[0].CenterW;
    look.y = 0.0f; // y-axis aligned, so project to xz-plane
    look = normalize(look);
    float3 right = cross(up, look);

	//
	// Compute triangle strip vertices (quad) in world space.
	//
    float halfWidth = 0.5f * gin[0].SizeW.x;
    float halfHeight = 0.5f * gin[0].SizeW.y;
	
    float4 v[4];
    v[0] = float4(gin[0].CenterW + halfWidth * right - halfHeight * up, 1.0f);
    v[1] = float4(gin[0].CenterW + halfWidth * right + halfHeight * up, 1.0f);
    v[2] = float4(gin[0].CenterW - halfWidth * right - halfHeight * up, 1.0f);
    v[3] = float4(gin[0].CenterW - halfWidth * right + halfHeight * up, 1.0f);

	//
	// Transform quad vertices to world space and output 
	// them as a triangle strip.
	//
	
    float2 texC[4] =
    {
        float2(0.0f, 1.0f),
		float2(0.0f, 0.0f),
		float2(1.0f, 1.0f),
		float2(1.0f, 0.0f)
    };
	
    GeoOut gout;
	[unroll]
    for (int i = 0; i < 4; ++i)
    {
        gout.PosH = mul(v[i], gViewProj);
        gout.PosW = v[i].xyz;
        gout.NormalW = look;
        gout.TexC = texC[i];
        gout.PrimID = primID;
		
        triStream.Append(gout);
    }
}

float4 PS(GeoOut pin) : SV_Target
{
    float3 uvw = float3(pin.TexC, pin.PrimID % 3);
    MaterialData matData = gMaterialData[gMatIndex];
    Texture2DArray gTreeMapArray = ResourceDescriptorHeap[matData.DiffuseMapIndex];
    float4 diffuseAlbedo = gTreeMapArray.Sample(gsamAnisotropicWrap, uvw) * matData.DiffuseAlbedo;
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
