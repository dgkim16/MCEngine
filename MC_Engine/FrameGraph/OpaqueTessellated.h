#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class OpaqueTessellated : public RenderPass
{
public:
	explicit OpaqueTessellated(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "OpaqueTessellated"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::OpaqueTessellated].empty()) return;

		const char* psoName =
			mEngine.IsWireFrame()
			? (mEngine.IsMsaa() ? "tessellation_wireframe_MSAA" : "tessellation_wireframe")
			: (mEngine.IsMsaa() ? "tessellation_MSAA" : "tessellation");
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawLayer(ctx.cmdList, RenderLayer::OpaqueTessellated, "opaqueTessellated Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

