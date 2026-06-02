#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class AlphaTestedPass : public RenderPass
{
public:
	explicit AlphaTestedPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "AlphaTested"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::AlphaTested].empty()) return;

		const char* psoName = mEngine.IsMsaa() ? "alphaTested_MSAA" : "alphaTested";
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawLayer(ctx.cmdList, RenderLayer::AlphaTested, "Alpha Tested Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

