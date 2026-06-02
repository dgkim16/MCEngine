#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class MirrorsPass : public RenderPass
{
public:
	explicit MirrorsPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "Mirrors"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::Mirrors].empty()) return;

		ctx.cmdList->OMSetStencilRef(1);

		const char* psoName = mEngine.IsMsaa() ? "markStencilMirrors_MSAA" : "markStencilMirrors";
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawLayer(ctx.cmdList, RenderLayer::Mirrors, "Mirror Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

