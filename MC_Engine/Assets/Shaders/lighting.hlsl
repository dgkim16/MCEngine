#define HLSL_CODE 1
#ifndef SHARED_TYPES
#include "SharedTypes.h"
#endif 
#define PI 3.141592


// Schlick gives an approximation to Fresnel reflectance (see pg. 233 "Real-Time Rendering 3rd Ed.").
// R0 = ( (n-1)/(n+1) )^2, where n is the index of refraction.
float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
    float cosIncidentAngle = saturate(dot(normal, lightVec)); // same as max(dot(normal,lightVec),0.0f);

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
// this is Luna's version of normalized Phong BRDF: 1/PI diffuse, (m+8)/8 lobe, (1-F_ split, Fresnel on N-L
// all vector params must be normalized prior to calling this fxn except data inside LightData
// this is NOT original BlinnPhong (no division of Specular term by cos(theta))
float4 LunaPhongBRDF(float4 diffuseAlbedo, float4 ambientLight, float3 fresnelR0, float3 normal, float3 toEyeW, float3 posW, float shininess, LightData light, float3 lightDir)
{   
    // C_ambient
    float4 ambient = ambientLight * diffuseAlbedo * float4(light.mLightColor, 1.0f);
    // C_diffues
    float3 lightVec = normalize(lightDir);
    float Intensity = max(dot(lightVec, normal), 0.0f) * light.mLightIntensity;
    if (light.mLightType == 1) // POINT LIGHT
    {
        float dist = distance(light.mLightPosition, posW);
        if(dist > light.mLightRadius)
            Intensity *= Attenuation(light.mAttenuationDist, dist - light.mLightRadius);
    }
    // C_specular
    float m = shininess * 256.0f;
    float3 halfvec = normalize(toEyeW + lightVec); // for Blinn
    // float3 reflvec = 2.0f * dot(normal, lightVec) * normal - lightVec; // for Phong
    
    float roughnessFactor = (m + 8.0f) / 8.0f * pow(max(dot(halfvec, normal), 0.0f), m); // Blinn
    // float roughnessFactor = (m + 8.0f) / 8.0f * pow(max(dot(reflvec, toEyeW), 0.0f), m); // Phong
    float3 fresnelFactor = SchlickFresnel(fresnelR0, halfvec, lightVec); // fixed to use halfvec instead of toEyeW
    float3 lambert = diffuseAlbedo.rgb / PI;
    float3 Kd = (float3(1.0f,1.0f,1.0f) - fresnelFactor) * lambert;
    
    // specAlbedo = Ks * pow(cos(sigm),alpha)
    float3 specAlbedo = fresnelFactor * roughnessFactor; // Rf(alpha_h) * (m+8)/8 * (n⋅d)^m 
    specAlbedo = specAlbedo / (specAlbedo + 1.0f);
    // specAlbedo /= saturate(dot(lightVec, normal));
    float4 addedColors = saturate(float4((Kd.rgb + specAlbedo) * Intensity, 1.0f));
    float4 litColor = addedColors + ambient;
    return litColor;
}
// float3 Kd = float3(1.0f) - fresnelFactor;

float4 classicBlinnPhong(float4 diffuseAlbedo, float4 ambientLight, float3 fresnelR0, float3 normal, float3 toEyeW, float3 posW, float shininess, LightData light, float3 lightDir)
{
    float3 lightVec = normalize(lightDir);
    float ndotl = max(dot(lightVec, normal), 0.0f);

    // Radiance arriving at this point = intensity * attenuation. No n·l baked in.
    float I = 1.0f;  // light.mLightIntensity;
    if (light.mLightType == 1) // POINT LIGHT
    {
        float dist = distance(light.mLightPosition, posW);
        if (dist > light.mLightRadius)
            I *= Attenuation(light.mAttenuationDist, dist - light.mLightRadius);
    }
    // Diffuse:  kd * (n·l)            -- foreshortened
    float3 diffuse = diffuseAlbedo.rgb * ndotl;

    // Specular: ks * (n·h)^alpha      -- NOT foreshortened (the defining property)
    float alpha = shininess * 256.0f; // your shininess[0,1] -> exponent remap
    float3 halfVec = normalize(toEyeW + lightVec); // assumes toEyeW is already normalized
    float ndoth = max(dot(halfVec, normal), 0.0f);
    float3 specular = (ndotl <= 0.0f) ? 0.0f : fresnelR0 * pow(ndoth, alpha);

    // C = ambient + I * ( kd*(n·l) + ks*(n·h)^alpha )
    float3 rgb = ambientLight.rgb * diffuseAlbedo.rgb + (diffuse + specular) * I;
    return float4(saturate(rgb), 1.0f);
}

float4 PhongBRDF()
{
    return float4(1.0f,1.0f,1.0f,1.0f);
}