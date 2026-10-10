#pragma once
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include "Legacy/MCEngine.h"
#include "Legacy/MCScene.h"

class OpaqueTessellated : public RenderPass
{
public:
	explicit OpaqueTessellated(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "OpaqueTessellated"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.ForwardPassWriteTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (!scene || scene->layers[(int)RenderLayer::OpaqueTessellated].empty()) return;

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

