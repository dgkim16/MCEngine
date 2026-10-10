#pragma once
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"

class AlphaTestedTreeSpritesPass : public RenderPass
{
public:
	explicit AlphaTestedTreeSpritesPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "AlphaTestedTreeSprites"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (!scene || scene->layers[(int)RenderLayer::AlphaTestedTreeSprites].empty()) return;

		const char* psoName =
			mEngine.IsWireFrame()
			? (mEngine.IsMsaa() ? "treeSprites_wireframe_MSAA" : "treeSprites_wireframe")
			: (mEngine.IsMsaa() ? "treeSprites_MSAA" : "treeSprites");
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawLayer(ctx.cmdList, RenderLayer::AlphaTestedTreeSprites, "treeSprites Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

