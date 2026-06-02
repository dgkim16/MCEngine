#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class GrassInstancedPass : public RenderPass
{
public:
	explicit GrassInstancedPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "GrassInstanced"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(ID3D12GraphicsCommandList* cmdList) override {
		auto* gc = mEngine.Scenes().GetActive()->GetModule<MCGrassCullingModule>();
		if (gc && gc->useGpuCulling) return;

		const char* psoName = 
			mEngine.IsWireFrame()
			? (mEngine.IsMsaa() ? "grass_instanced_wireframe_MSAA" : "grass_instanced_wireframe")
			: (mEngine.IsMsaa() ? "grass_instanced_MSAA" : "grass_instanced");
		cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawInstancedLayer(cmdList, RenderLayer::GrassInstanced, true, "Grass Instanced Pass (CPU)");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};
