#pragma once
#include "Renderer/D3D12/FrameGraph/RenderPass.h"
#include <functional>
#include <string>

class LambdaPass : public RenderPass {
public:
	using SetupFn = std::function<void(RenderPassBuilder&)>;
	// using ExecuteFn = std::function<void(ID3D12GraphicsCommandList*)>;
	using ExecuteFn = std::function<void(FgRenderContext&)>;

	LambdaPass(std::string typeName, SetupFn setup, ExecuteFn execute)
		: mTypeName(typeName), mSetup(std::move(setup)), mExecute(std::move(execute)) {}

	const char* TypeName() const override { return mTypeName.c_str(); }
	void Setup(RenderPassBuilder& b) override { mSetup(b); }
	void Execute(FgRenderContext& ctx) override { mExecute(ctx); }

private:
	std::string mTypeName;
	SetupFn mSetup;
	ExecuteFn mExecute;
};

