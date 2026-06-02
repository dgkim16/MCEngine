#pragma once
#include "RenderPass.h"
#include "../MCEngine.h"
#include "../MCScene.h"

class OpaqueInstancedPass : public RenderPass
{
public:
	explicit OpaqueInstancedPass(MCEngine& engine) : mEngine(engine) {};
	const char* TypeName() const override { return "OpaqueInstanced"; }
	void Setup(RenderPassBuilder& b) override {
		b.Write(mEngine.SceneColor(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		b.Write(mEngine.SceneDepth(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	void Execute(FgRenderContext& ctx) override {
		auto scene = mEngine.Scenes().GetActive();
		if (scene->layers[(int)RenderLayer::OpaqueInstanced].empty()) return;

		const char* psoName =
			mEngine.IsWireFrame()
			? (mEngine.IsMsaa() ? "opaque_instanced_tess_wireframe_MSAA" : "opaque_instanced_tess_wireframe")
			: (mEngine.IsMsaa() ? "opaque_instanced_tess_MSAA" : "opaque_instanced_tess");
		ctx.cmdList->SetPipelineState(mEngine.GetPSO(psoName));

		mEngine.DrawInstancedLayer(ctx.cmdList, RenderLayer::OpaqueInstanced, /*useGrass=*/false, "opaque Instanced Pass");
	}
private:
	MCEngine& mEngine;
	// no PSO caching
};

