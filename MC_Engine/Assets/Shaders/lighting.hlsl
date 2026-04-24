#define HLSL_CODE 1
#ifndef SHARED_TYPES
#include "SharedTypes.h"
#endif 
#define PI 3.141592


// Schlick gives an approximation to Fresnel reflectance (see pg. 233 "Real-Time Rendering 3rd Ed.").
// R0 = ( (n-1)/(n+1) )^2, where n is the index of refraction.
float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
    float cosIncidentAngle = saturate(dot(normal, lightVec));

    float f0 = 1.0f - cosIncidentAngle;
    float3 reflectPercent = R0 + (1.0f - R0) * (f0 * f0 * f0 * f0 * f0);

    return reflectPercent;
}

// attenDist defined by Light, attenLength defined by distance going beyond radius
float Attenuation(float attenDist, float attenLength)
{
    return smoothstep(0.0f, 1.0f, attenLength/attenDist);   // might need to flip 0 and 1
}

// shininess must be between 0 and 1 (1.0f-roughness)
float4 SimpleBlinnPhong(float4 diffuseAlbedo, float4 ambientLight, float3 fresnelR0, float3 normal, float3 toEyeW, float3 posW, float shininess, LightData light)
{
    // C_ambient
    float4 ambient = ambientLight * diffuseAlbedo * float4(light.mLightColor, 1.0f);
    // C_diffues
    float3 lightVec = light.mLightDirection;
    lightVec = normalize(lightVec);
    float Intensity = max(dot(lightVec, normal), 0.0f) * light.mLightIntensity; // used to use * 3.0
    if (light.mLightType == 1) // POINT LIGHT
    {
        float dist = distance(light.mLightPosition, posW);
        if(dist > light.mLightRadius)
            Intensity *= Attenuation(light.mAttenuationDist, dist - light.mLightRadius);
    }
    // C_specular
    float m = shininess * 256.0f;
    float3 halfvec = normalize(toEyeW + lightVec);
    
    float roughnessFactor = (m + 8.0f) / 8.0f * pow(max(dot(halfvec, normal), 0.0f), m);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, normal, lightVec);
    float3 lambert = diffuseAlbedo.rgb / PI;
    float3 Kd = (float3(1.0f,1.0f,1.0f) - fresnelFactor) * lambert;
    
    float3 specAlbedo = fresnelFactor * roughnessFactor; // Rf(alpha_h) * (m+8)/8 * (n⋅d)^m 
    specAlbedo = specAlbedo / (specAlbedo + 1.0f);
    float4 addedColors = saturate(float4((Kd.rgb + specAlbedo) * Intensity, 1.0f));
    float4 litColor = addedColors + ambient;
    return litColor;
}
// float3 Kd = float3(1.0f) - fresnelFactor;