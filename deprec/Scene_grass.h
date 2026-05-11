#pragma once
#include "MCScene.h"
class Scene_grass : public MCScene
{
public:
	Scene_grass() { name = "Grass"; }
	void Load(MCEngine& engine) override;
	void Activate(MCEngine& engine) override;
	void RebindCachedPointers(MCEngine& engine) override;
	void Deactivate(MCEngine& engine) override;
	void Update(MCEngine& engine, float dt) override;
	void OnImGui(MCEngine& engine) override;
	void ResetSceneResources() override;

private:
	void BuildGeometry(MCEngine& engine);
	void BuildMaterials(MCEngine& engine);
	void BuildRenderItems(MCEngine& engine);
	void BuildInstanceCells();
	void PopulateGrassInstances();  // called from BuildRenderItems and Activate

	void BuildGpuCullingBuffers(MCEngine& engine);

public:
	float grassWidth = 0.4f;			// serialize
	float grassHeight = 1.0f;			// serialize
	int grassCountWidth = 500;			// serialize
	int grassCountDepth = 500;			// serialize
	float grassCoverageWidth = 500.0f;	// serialize
	float grassCoverageDepth = 500.0f;	// serialize
	float grassSharpness = 0.9f;		// serialize

	RenderItem* mGrassRitem = nullptr;
	RenderItem* mPlaneRitem = nullptr;

	MCMaterial* mPlaneMaterial = nullptr;
	MCMaterial* mGrassMaterial = nullptr;
	bool equalColor = true;

	int numCellsX = 5;
	int numCellsZ = 5;

	// Compute Shader for culling
	bool useGpuCulling = false;


	MCBufferResource mGrassFullInstanceBuffer;   // SRV for cull CS
	MCBufferResource mGrassIndirectArgsBuffer;   // ExecuteIndirect source
	MCBufferResource mGrassVisibleBuffer;        // CS UAV write, VS SRV read
	MCBufferResource mGrassCounterBuffer;        // CS UAV write, CopyBufferRegion src
	MCBufferResource mGrassCounterResetBuffer;   // CopyBufferRegion src (permanent upload)

	/*
	ComPtr<ID3D12Resource> mGrassFullInstanceBuffer;  // default heap, written once
	ComPtr<ID3D12Resource> mGrassFullInstanceUpload;  // upload staging (freed after load)
	ComPtr<ID3D12Resource> mGrassIndirectArgsBuffer;  // default heap, D3D12_DRAW_INDEXED_ARGUMENTS
	ComPtr<ID3D12Resource> mGrassIndirectArgsUpload;  // upload staging (freed after load)
	ComPtr<ID3D12Resource> mGrassVisibleBuffer;       // UAV output from CS
	ComPtr<ID3D12Resource> mGrassCounterBuffer;       // single uint counter (UAV)
	ComPtr<ID3D12Resource> mGrassCounterResetBuffer;  // upload heap, contains UINT 0
	*/

	std::array<std::unique_ptr<UploadBuffer<GrassCullCB>>, gNumFrameResources> mGrassCullCB;  // updated per frame
	UINT mTotalGrassInstances = 0;
};

