#pragma once
#include "SharedTypes.h"
#include "Renderer/D3D12/MC_Types.h"
#include "Renderer/D3D12/UploadHelper.h"
#include "Renderer/D3D12/Singleton.h"
#include "d3d12.h"
#include "Renderer/D3D12/FrameResource.h"
#include "Renderer/D3D12/ConstExpressionValues.h"
#include <memory>
#include <vector>
#include <DirectXCollision.h>

class BarrierManager;

/*
struct LightData {
    float3 mLightColor;
    float mLightIntensity;
    float3 mLightDirection;     // spotlight, directional light
    float mAttenuationDist;     // punctual lights only (spot & point)
    float3 mLightPosition;      // punctual lights only (spot & point)
    float mLightRadius;         // punctual lights only (spot & point)
    float4x4 mShadowTransform;  // LightPos * mShadowTransform = R, use R.xy as uv on shadowmap (sm.xy), and compare with R.z. if R.z greater, shaded area. if smaller, not shaded area.

    int mLightType;             // 0 directional, 1 point, 2 spot, 3 count
    int mCastShadow;            // 0 is no-cast. 1 is caster.
    int mShadowMapIndex;
};
*/

enum class LightType {
    Directional = 0,
    Point,
    Spot,
    LightTypeCount
};

enum class ShadowMapVolumeType {
    NOT_SHADOW_CASTER = 0,
    ORTHO_LIGHT,
    ORTHO_VIEW,
    PERSP_VIEW,
    COUNT
};

class MCLight
{
public:
    MCLight(LightType type, bool castShadow = false, ShadowMapVolumeType volumeType = ShadowMapVolumeType::NOT_SHADOW_CASTER, DirectX::XMFLOAT3 lightPos = { 0.0f,0.0f,0.0f }, DirectX::XMFLOAT3 lightDir = { 0.01f,0.01f,0.01f }, float attenDist = 1.0f, float radius = 1.0f)
        : mIsDirty(3)
    {
        mData.mLightPosition = lightPos;
        mData.mLightDirection = lightDir;
        mData.mAttenuationDist = attenDist;
        mData.mLightRadius = radius;
        mData.mLightType = (int)type;
        mData.mCastShadow = castShadow;
        mData.mShadowMapIndex = -1;
        Initialize(volumeType);   
    }
    MCLight(LightData ld, ShadowMapVolumeType volumeType = ShadowMapVolumeType::NOT_SHADOW_CASTER) : mData(ld) 
    {
        Initialize(volumeType);
        SetDepthBias(ld.depthBias);
    }
    ~MCLight() {
        mShadowMap.reset();
    }
    MCLight(const MCLight& rhs) = delete;
    MCLight(MCLight&& rhs) noexcept 
        : mData(rhs.mData)
        , mIsDirty{ gNumFrameResources }
        , mShadowMapResolution(rhs.mShadowMapResolution)
        , mShadowMapFrustumRadius(rhs.mShadowMapFrustumRadius)
        , mShadowMap(std::move(rhs.mShadowMap))
        , mShadowMapVolumeType(rhs.mShadowMapVolumeType)
        , depthBias(rhs.depthBias)
    {
        rhs.mData.mShadowMapIndex = -1;
        rhs.mData.mCastShadow = 0;
    }
    MCLight& operator=(MCLight&& rhs) noexcept
    {
        if (this != &rhs) {
            mData = rhs.mData;
            mShadowMapResolution = rhs.mShadowMapResolution;
            mShadowMapFrustumRadius = rhs.mShadowMapFrustumRadius;
            mShadowMap = std::move(rhs.mShadowMap);
            rhs.mData.mShadowMapIndex = -1;
            rhs.mData.mCastShadow = 0;
            mIsDirty = gNumFrameResources;
            mShadowMapVolumeType = rhs.mShadowMapVolumeType;
        }
        return *this;
    }
    void Initialize(ShadowMapVolumeType volumeType);
    void MarkDirty() { mIsDirty = gNumFrameResources; }
    const int GetShadowMapFrustum() const { return mShadowMapFrustumRadius; }
    const int GetShadowMapResolution() const { return mShadowMapResolution; }
    void SetShadowMapVolumeType(ShadowMapVolumeType dest) { mShadowMapVolumeType = dest; MarkDirty(); }
    const ShadowMapVolumeType GetShadowMapVolumeType() const { return mShadowMapVolumeType; }
    const float GetDepthBias() const { return depthBias; }
    void SetDepthBias(float f) { depthBias = f; }
    const float GetWarpAmount() const { return warpAmount; }
    void SetWarpAmount(float f) { warpAmount = f; }
    const float GetPseudoNear() const { return pseudoNear; }
    void SetPseudoNear(float f) { pseudoNear = f; }
    const float GetPseudoFar() const { return pseudoFar; }
    void SetPseudoFar(float f) { pseudoFar = f; }
    const bool GetClipToScene() const { return clipToScene; }
    void SetClipToScene(bool b) { clipToScene = b; }
    
public:
    LightData mData;
    std::unique_ptr<MCTextureResource> mShadowMap;
    int mIsDirty = gNumFrameResources;
private:
    float warpAmount = -0.1f;
    float pseudoNear = 1.0f;    // PERSP_VIEW: z_n used by n_opt is max(camera near, this). Keeps a tiny camera near plane from over-warping
    float pseudoFar = 40.0f;    // PERSP_VIEW: z_f used by n_opt and the far end of the fitted body. Receivers past it are lit, not shadowed
    bool clipToScene = true;    // PERSP_VIEW: fit only the part of the view frustum inside the scene bounds
    float depthBias = 0.01f;
    int mShadowMapResolution = 1024;
    int mShadowMapFrustumRadius = 40;
    ShadowMapVolumeType mShadowMapVolumeType;
};


class MCLightManager : public Singleton<MCLightManager>
{
    friend class Singleton<MCLightManager>;
private:
    MCLightManager() = default;
public:
    void Shutdown();
    void AddLight(MCLight&& light) { mLights.push_back(std::move(light)); }
    void RemoveLight(MCLight& light);
    void RemoveLight(int idx);
    MCLight& GetLight(int idx) { return mLights[idx]; } // probably should be const, but I'm too lazy to write the setters...
    int LightCount() { return static_cast<int>(mLights.size()); }

    void Update(FrameResource& currFrameResource) {
        UpdateShadowMapPassCBs(currFrameResource);
        UpdateLightsDataSRVs(currFrameResource);
    }
    void UpdateShadowMapPassCBs(FrameResource& currFrameResource);
    void UpdateLightsDataSRVs(FrameResource& currFrameResource);

    void MarkAllLightsDirty(int dirty);
    void CreateShadowMapResources(ID3D12Device* md3dDevice, BarrierManager& barriers);
    
    void SetupShadowMapDSVSRVs();
    // world-space bounds of the shadowed geometry; PERSP_VIEW clips the view frustum to these before fitting
    void SetSceneBounds(const DirectX::BoundingBox& b) { mSceneBounds = b; mHasSceneBounds = true; }

    const float GetWarpDebug() const { return currentWarp; }

    static constexpr DXGI_FORMAT GetDepthShadowMapFormat() { return mDepthShadowMapFormat; }
    static constexpr DXGI_FORMAT GetDepthShadowMapDSVFormat() { return mDepthShadowMapDSVFormat; }
    static constexpr DXGI_FORMAT GetDepthShadowMapSRVFormat() { return mDepthShadowMapSRVFormat; }
private:
    std::vector<MCLight> mLights;
    DirectX::BoundingBox mSceneBounds; // this is bounding box of scene in world space
    bool mHasSceneBounds = false;
    static constexpr DXGI_FORMAT mDepthShadowMapFormat = DXGI_FORMAT_R32_TYPELESS;
    static constexpr DXGI_FORMAT mDepthShadowMapDSVFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr DXGI_FORMAT mDepthShadowMapSRVFormat = DXGI_FORMAT_R32_FLOAT;

private: // debugging only. remove later
    float currentWarp = 0.0f;
};