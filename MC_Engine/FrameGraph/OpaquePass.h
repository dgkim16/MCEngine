#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class OpaquePass : public RenderPass
{
public:
	explicit OpaquePass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "Opaque"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::Opaque].empty()) return;

		const char* psoName =
			mEngine.IsWireFrame()
			? (mEngine.IsMsaa() ? "opaque_wireframe_MSAA" : "opaque_wireframe")
			: (mEngine.IsMsaa() ? "opaque_MSAA" : "opaque");
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawLayer(ctx.cmdList, RenderLayer::Opaque, "opaque Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

