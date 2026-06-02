#pragma once
#include <cstdint>
#include <d3d12.h>
#include "../GpuTimer.h"
#include "FgRenderContext.h"
#include "FgBlackboard.h"

class FrameGraph;
class MCEngine;
class MCResource;

/*
* Note
* - Do not add GPU_SCOPED in the RenderPass body. 
*   FrameGraph adds it via PIXScopedEvent in Execute
*/
class RenderPassBuilder
{
public:
    RenderPassBuilder(FrameGraph* g, uint32_t pi);
    void Read(MCResource& res, D3D12_RESOURCE_STATES req);
    void Write(MCResource& res, D3D12_RESOURCE_STATES req);
    void Read(FgResourceHandle h, D3D12_RESOURCE_STATES req);
    void Write(FgResourceHandle h, D3D12_RESOURCE_STATES req);
    FgResourceHandle Create(const FgResourceDesc& desc);
    template <class T> T& AddBlackboard();   // forwards to mGraph's FgBlackboard.Add<T>()
    template <class T> T& GetBlackboard();    // forwards to .Get<T>()
    template <class T> T* TryGetBlackboard(); // forwards to .TryGet<T>()
private:
    FrameGraph* mGraph;
    uint32_t mPassIdx;
};

class RenderPass {
public:
    RenderPass() = default;
    RenderPass(const RenderPass&) = delete; // copy ctor
    RenderPass& operator=(const RenderPass&) = delete; // copy assign
    RenderPass(RenderPass&&) = delete; // move ctor
    RenderPass& operator=(RenderPass&&) = delete; // move assign
    virtual ~RenderPass() = default;

    virtual const char* TypeName() const = 0; // for ImGui labels & debug dumps. never used for lookups during Compile or Execute.
    virtual void Setup(RenderPassBuilder& b) = 0;
    virtual void Execute(FgRenderContext& ctx) = 0;
    virtual void OnImGui(MCEngine&) {};
    // member fields direct on dreived class.

    TimerID mTimerId = TimerID(-1);
};

