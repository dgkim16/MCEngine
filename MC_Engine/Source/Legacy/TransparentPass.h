#pragma once
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"

class TransparentPass : public RenderPass
{
public:
	explicit TransparentPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "Transparent"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (!scene || scene->layers[(int)RenderLayer::Transparent].empty()) return;
		const char* psoName =
			mEngine.IsMsaa() ? "transparent_MSAA" : "transparent";
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));
		mEngine.DrawLayer(ctx.cmdList, RenderLayer::Transparent, "Transparent Pass Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

