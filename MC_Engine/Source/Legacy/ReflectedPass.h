#pragma once
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"

class ReflectedPass : public RenderPass
{
public:
	explicit ReflectedPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "Reflected"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene && !scene->layers[(int)RenderLayer::Reflected].empty()) {
			const char* psoName =
				mEngine.IsWireFrame()
				? (mEngine.IsMsaa() ? "drawStencilReflections_wireframe_MSAA" : "drawStencilReflections_wireframe")
				: (mEngine.IsMsaa() ? "drawStencilReflections_MSAA" : "drawStencilReflections");
			ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));
			mEngine.DrawLayer(ctx.cmdList, RenderLayer::Reflected, "Reflected Pass");
		}
		ctx.cmdList->OMSetStencilRef(0); // must restore stencil ref
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

