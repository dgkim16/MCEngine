#pragma once
#include "MCSceneModule.h"
#include "RenderItem.h"      // RenderItem; transitively FrameResource → UploadBuffer, GrassCullCB, gNumFrameResources, <array>
#include "MC_Types.h"        // MCBufferResource
#include "MCMaterial.h"      // MCMaterial

class MCGrassCullingModule : public MCSceneModule {
public:
	MCGrassCullingModule() = default;
	~MCGrassCullingModule() override = default;

	const char* TypeName() const override { return "MCGrassCulling"; }

	void OnLoad      (MCEngine& engine, MCScene& scene) override;
	void OnActivate  (MCEngine& engine, MCScene& scene) override;
	void OnDeactivate(MCEngine& engine, MCScene& scene) override;
	void OnUpdate    (MCEngine& engine, MCScene& scene, float dt) override;
	void OnDraw      (MCEngine& engine, MCScene& scene) override;
	void OnImGui     (MCEngine& engine, MCScene& scene) override;
	void ToJson(nlohmann::json& j) const override;
	void FromJson(const nlohmann::json& j) override;

private:
	void CacheMaterialPointers(MCEngine& engine);
	void PopulateGrassInstances(MCEngine& engine);
	void BuildInstanceCells(MCEngine& engine);
	void BuildGpuCullingBuffers(MCEngine& engine);

	std::string grassRitemName = "quad";
	std::string planeRitemName = "grid";

public:
	int numCellsX = 5;
	int numCellsZ = 5;
	// float grassWidth = 0.4f;			// use value set by mesh params, not scenemodule params
	// float grassHeight = 1.0f;		// use value set by mesh params, not scenemodule params
	// float grassSharpness = 0.9f;		// use value set by mesh params, not scenemodule params
	int grassCountWidth = 500;			// serialize
	int grassCountDepth = 500;			// serialize
	float grassCoverageWidth = 500.0f;	// serialize
	float grassCoverageDepth = 500.0f;	// serialize

	// D9 9B — single source of truth for blade w/h/sharpness is the grass-blade
	// mesh's MCMeshSource::GrassPatchParams. OnLoad caches the handle so OnDraw
	// and OnImGui can reach the params via engine.Meshes().GetSource(mBladeMeshHandle).
	MeshHandle mBladeMeshHandle = 0;

	RenderItem* mGrassRitem = nullptr;
	RenderItem* mPlaneRitem = nullptr;

	MCMaterial* mPlaneMaterial = nullptr;
	MCMaterial* mGrassMaterial = nullptr;
	bool equalColor = true;

	// Compute Shader for culling
	bool useGpuCulling = false;
	MCBufferResource mGrassFullInstanceBuffer;   // SRV for cull CS
	MCBufferResource mGrassIndirectArgsBuffer;   // ExecuteIndirect source
	MCBufferResource mGrassVisibleBuffer;        // CS UAV write, VS SRV read
	MCBufferResource mGrassCounterBuffer;        // CS UAV write, CopyBufferRegion src
	MCBufferResource mGrassCounterResetBuffer;   // CopyBufferRegion src (permanent upload)

	std::array<std::unique_ptr<UploadBuffer<GrassCullCB>>, gNumFrameResources> mGrassCullCB;  // updated per frame
	UINT mTotalGrassInstances = 0;
};