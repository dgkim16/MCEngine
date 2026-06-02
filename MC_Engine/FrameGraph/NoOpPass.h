#pragma once
#include "RenderPass.h"
#include <../MC_Types.h>
class NoOpPass : public RenderPass
{
public:
    explicit NoOpPass(MCEngine&) {}
    const char* TypeName() const override { return "NoOpPass"; }
    void Setup(RenderPassBuilder&) override {}  // declares nothing
    void Execute(FgRenderContext& ctx) override {}  // does nothing
};

