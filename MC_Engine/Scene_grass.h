#pragma once
#include "Scene.h"
class Scene_grass : public Scene
{
public:
	Scene_grass() { name = "Grass"; }
	void Load(MCEngine& engine) override;
	void Activate(MCEngine& engine) override;
	void Deactivate(MCEngine& engine) override;
	void Update(MCEngine& engine, float dt) override;
	void Scene_IMGUI(MCEngine& engine) override;
	void ResetSceneResources() override;
private:
	void BuildGeometry(MCEngine& engine);
	void BuildMaterials(MCEngine& engine);
	void BuildRenderItems(MCEngine& engine);
	void BuildInstanceCells();

	void BuildGpuCullingBuffers(MCEngine& engine);

public:
	float grassWidth = 0.4f;
	float grassHeight = 1.0f;
	int grassCountWidth = 500;
	int grassCountDepth = 500;
	float grassCoverageWidth = 500.0f;
	float grassCoverageDepth = 500.0f;
	float grassSharpness = 0.9f;

	RenderItem* mGrassRitem = nullptr;
	RenderItem* mPlaneRitem = nullptr;

	Material* mPlaneMaterial = nullptr;
	Material* mGrassMaterial = nullptr;
	bool equalColor = true;

	int numCellsX = 5;
	int numCellsZ = 5;

	// Compute Shader for culling
	bool useGpuCulling = false;


	MCBuffer mGrassFullInstanceBuffer;   // SRV for cull CS
	MCBuffer mGrassIndirectArgsBuffer;   // ExecuteIndirect source
	MCBuffer mGrassVisibleBuffer;        // CS UAV write, VS SRV read
	MCBuffer mGrassCounterBuffer;        // CS UAV write, CopyBufferRegion src
	MCBuffer mGrassCounterResetBuffer;   // CopyBufferRegion src (permanent upload)

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

