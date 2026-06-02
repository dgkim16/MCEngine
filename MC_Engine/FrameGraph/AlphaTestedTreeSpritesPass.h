#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class AlphaTestedTreeSpritesPass : public RenderPass
{
public:
	explicit AlphaTestedTreeSpritesPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "AlphaTestedTreeSprites"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::AlphaTestedTreeSprites].empty()) return;

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

