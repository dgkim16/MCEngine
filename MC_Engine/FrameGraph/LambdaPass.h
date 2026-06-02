#pragma once
#include "RenderPass.h"
#include <functional>

class LambdaPass : public RenderPass {
public:
	using SetupFn = std::function<void(RenderPassBuilder&)>;
	// using ExecuteFn = std::function<void(ID3D12GraphicsCommandList*)>;
	using ExecuteFn = std::function<void(FgRenderContext&)>;

	LambdaPass(const char* typeName, SetupFn setup, ExecuteFn execute)
		: mTypeName(typeName), mSetup(std::move(setup)), mExecute(std::move(execute)) {}

	const char* TypeName() const override { return mTypeName; }
	void Setup(RenderPassBuilder& b) override { mSetup(b); }
	void Execute(FgRenderContext& ctx) override { mExecute(ctx); }

private:
	const char* mTypeName;
	SetupFn mSetup;
	ExecuteFn mExecute;
};

