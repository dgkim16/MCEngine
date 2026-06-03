# MCEngine
![MCEngine](MC_Engine/MC.ico)

A DirectX 12 rendering engine I am building to learn low-level graphics engine architecture.

<!-- Hero image: save a representative screenshot (recommended: the grass scene with post-FX on)
     as MC_Engine/Assets/readme/hero.png, then uncomment the line below. -->
 

*Active learning project. Windows + D3D12 only. API unstable; no release cadence.*

## Features

- Runtime HLSL compilation targeting Shader Model 6.x through DXC, with an on-disk shader cache keyed by source path and entry point.
- FrameGraph that orchestrates each frame as a graph of Render passes (per-frame pass culling, transient creation and aliasing).
- Per-layer PSO binning across eight render layers (Opaque, Mirrors, Reflected, Transparent, AlphaTested, Shadow, AlphaTestedTreeSprites, OpaqueTessellated).
- Post-processing Render passes that uses Compute Shaders via `dispatch` calls. (i.e. Blur, Sobel)
- Assimp-driven model import through a thin wrapper (`Common/MyImporter.h`).
- In-engine ImGui toolchain on the docking branch, including a live descriptor-heap visualizer.
- Explicit resource state-transition batching through `BarrierManager`.
- Stable, predictable GPU descriptor indices through `DescHeapManager` that are dynamicity-tiered.

## Build and run

Prerequisites:

- Visual Studio 2022 with the *Desktop development with C++* workload (platform toolset v143).
- Windows 10 SDK 10.0 or newer (whichever ships with VS2022 is sufficient).
- A Direct3D 12-capable GPU.
- NuGet — used to restore the `WinPixEventRuntime` package referenced by `MC_Engine/packages.config`. The Visual Studio NuGet integration handles this automatically on first build; on the command line, use `msbuild -t:Restore` (shown below) or run `nuget restore MC_Engine\MC_Engine.sln` first.
- The `WinPixEventRuntime` NuGet package (`WinPixEventRuntime` 1.0.240308001) — pulled by the restore step above. It supplies the PIX runtime headers and `WinPixEventRuntime.lib` linked by the engine for GPU/CPU event instrumentation. Captures themselves are taken with the standalone PIX on Windows tool from Microsoft.

Build from the command line:

```
msbuild MC_Engine\MC_Engine.sln -t:Restore;Build /p:Configuration=Release /p:Platform=x64
```

Or open `MC_Engine\MC_Engine.sln` in Visual Studio and build `x64 | Release` (NuGet packages restore automatically on first build).

Run:

```
MC_Engine\x64\Release\MC_main.exe
```

The build copies `dxcompiler.dll`, `dxil.dll`, and the Assimp runtime next to the EXE automatically. If you move the EXE, copy those DLLs from `MC_Engine\dll files\` alongside it.

## Repository layout

```
01_repo/
├── MC_Engine/
│   ├── Assets/                  # HLSL shaders, DDS textures, fonts, models
│   ├── Common/                  # Base D3D12 framework (D3DApp, FrameResource, GeometryGenerator)
│   ├── FrameGraph/              # FrameGraph & Render Pass source files (FrameGraph*, Fg*, *Pass)
│   ├── Includes/                # Vendored headers: Assimp, DXC, ImGui
│   ├── Libs/                    # Vendored static libs
│   ├── dll files/               # Runtime DLLs copied next to the EXE
│   ├── packages/                # NuGet packages (WinPixEventRuntime)
│   ├── MCEngine.cpp / .h        # Engine class: renderer state, frame resources, render items
│   ├── MC_PipelineManager.cpp   # PSO and root signature construction
│   ├── ForwardPass.cpp          # Main render loop; layer iteration and draw submission
│   ├── DescHeapManager.cpp / .h # Descriptor heap allocator
│   ├── BarrierManager.cpp / .h  # Resource state-transition batching
│   ├── ShaderLib*.cpp           # Runtime HLSL compilation cache (DXC/FXC)
│   ├── MC_imgui*.cpp            # ImGui integration and descriptor-heap visualizer
│   └── MC_Engine.sln
└── README.md
```

## Architecture at a glance

The engine is layered.

**Common framework (`MC_Engine/Common/`).** Provides the D3D12 base class `D3DApp` taken from Frank Luna's demos: window creation, device and swap-chain setup, command queue, triple-buffered command allocators, fence-based CPU/GPU synchronization, and per-frame constant buffers (pass, object, material). Also hosts procedural geometry (`GeometryGenerator`), math helpers (`MathHelper`), a DDS texture loader, an Assimp wrapper (`MyImporter`), and upload-heap staging utilities.

**MCEngine (`MC_Engine/MCEngine.cpp`).** The concrete renderer. Owns all descriptor heaps, root signatures, PSOs, the geometry map, the material map, the texture map, and the render-item list. Render items are tagged with a layer enum; `ForwardPass.cpp` iterates layers in fixed order and swaps PSOs at layer boundaries. A dedicated ImGui descriptor heap and an off-screen UAV chain for post-processing sit alongside the main SRV heap.

**Shaders (`MC_Engine/Assets/Shaders/`).** All HLSL is compiled at runtime by DXC against Shader Model 6.x. Compiled PDBs land in `MC_Engine/HLSL PDB/` for debugger attachment.

## Notable systems

### DescHeapManager

`MC_Engine/DescHeapManager.cpp` — central allocator for the GPU-visible CBV/SRV/UAV heap. Enforces one invariant: every GPU-visible descriptor has a stable, predictable index for the lifetime of the resource it describes. Root signatures and shader code bake those indices; without the invariant, a reallocation after a resize silently breaks sampling for every view that moved. The manager addresses allocation, indexing, and lifetime as distinct concerns rather than one opaque heap.

### BarrierManager

`MC_Engine/BarrierManager.cpp` — batches `D3D12_RESOURCE_BARRIER` records and flushes them at sync boundaries instead of emitting transitions per call site. Eliminates redundant transitions and avoids interleaved submission patterns the driver handles poorly. Current scope is intentionally narrow; broader resource-lifetime abstraction is on the roadmap.

### FrameGraph

`MC_Engine/FrameGraph/FrameGraph.cpp` — orchestrates the frame as a graph of passes rather than a hand-ordered command sequence. Each `RenderPass` declares the resources it reads and writes in a `Setup` step; `Compile` derives execution order from those declarations, culls passes whose outputs go unused, computes transient-resource lifetimes, and aliases their memory (greedy interval coloring per heap class) so short-lived targets share allocations. `Execute` then replays the passes, inserting every state transition through `BarrierManager` and wrapping each pass in a PIX and GPU-timer scope. Adding a pass no longer means re-reasoning about global ordering and barriers — the pass states its own reads and writes, and the graph does the rest. Active workstream: it already subsumes the post-processing compute chain (Gaussian blur, Sobel, alpha fixup) and is migrating geometry off the legacy `MCEngine::ForwardPass`; the two paths coexist until that completes.

## Roadmap

Three phases against a v3.0 target.

### Phase 1 — Foundations 

Instrumentation and abstractions every later phase assumes. The first two items have shipped; the remainder is the active work surface.

- ✓ Explicit resource state-transition batching through `BarrierManager`. Zero raw `ResourceBarrier` call sites outside the manager. State-tracking `CommandContext` extension (automatic `from`-state inference, cross-callsite merging, split barriers) is deferred until a measured cost demands it.
- ✓ JSON scene serialization. Content-hashed 64-bit asset handles back render-item references; `MCMaterialManager` / `MCMeshSourceManager` / `MCTextureManager` own asset lifecycle; `migrations.json` resolves renames at load. Save, close, reopen, load → the scene is identical, and a hand-edit to the JSON survives a reload.
- ✓ ImGui scene editor surface — outliner + inspector + file menu (New / Open / Save / Save As) + ImGuizmo translation gizmo.
- ✓ GPU timestamp queries feeding rolling averages into ImGui UI. Every major pass reports GPU milliseconds. ~~cross-checked against PIX.~~
- ✓ Frame graph (render graph). Passes declare reads and writes; the graph computes execution order, transient-resource lifetimes (with aliasing), and barrier insertion automatically (using `BarrierManager`). Replaces hand-batched pass scheduling in `MCEngine::ForwardPass` and the post-process compute chain.

### Phase 2 — Visual quality *(in progress)*

- Cook-Torrance metallic-roughness PBR, validated against Filament's matball at matched inputs.
- Image-based lighting: diffuse irradiance cubemap, specular prefilter, BRDF LUT.
- Cascaded shadow maps, four cascades with 5×5 rotated-Poisson PCF.
- Clustered-forward lighting on a 16×9×24 grid, point lights first.
- GTAO at half resolution with temporal and spatial denoise, integrated into indirect diffuse.
- Intel Sponza 2022 at locked 60 fps / 1080p, lit by the full stack.

### Phase 3 — Architecture and release

- Split into `MCCore` (static library, headless), `MCEditor` (ImGui host), and `MCRuntime` (shippable executable that never links ImGui or the debug layer).
- Runtime export — an editor menu entry produces a `.zip` a reviewer downloads, unzips, and double-clicks.
- Archetype-based ECS replacing `std::unordered_map<std::string, RenderItem>`. `view<Transform>` over 100k entities under 1 ms.
- Offline asset cooker: glTF 2.0 and PNG/HDR in, meshoptimizer-optimized binaries and BC-compressed DDS out. `MCRuntime` ships with zero Assimp.
- Intel Sponza 2022 demo scene with scripted camera flythrough.
- `v1.0` tag with a 90-second demo reel.

## References

- Frank Luna, *Introduction to 3D Game Programming with DirectX 12* — starting-point sample structure.
- Microsoft **MiniEngine** (DirectX-Graphics-Samples) — resource management patterns.
- Microsoft **DirectX-Graphics-Samples** — DXC usage and bindless patterns.
- Jason Gregory, *Game Engine Architecture* — subsystem decomposition.
- Fabian Giesen, *The ryg blog* — GPU-side reasoning and micro-architecture intuition. https://fgiesen.wordpress.com/
- Yuriy O'Donnell, *FrameGraph: Extensible Rendering Architecture in Frostbite* — Frame Graph design patterns https://www.gdcvault.com/play/1024612/FrameGraph-Extensible-Rendering-Architecture-in

# Sources
- 다람디.obj / .dds — 3d model and texuture generated via Meshy.ai using character from [arca.live](https://arca.live/e/50851?target=title&keyword=%EB%8B%A4%EB%9E%8C%EB%94%94&p=1) emoji.
- all other textures files — directly taken from Frank Luna's demo samples.

## License
MIT.
