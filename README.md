# MCEngine
![MCEngine](MC_Engine/MC.ico)

A DirectX 12 rendering engine I am building to learn low-level graphics engine architecture.

<!-- Hero image: save a representative screenshot (recommended: the grass scene with post-FX on)
     as MC_Engine/Assets/readme/hero.png, then uncomment the line below. -->
 

*Active learning project. Windows + D3D12 only. API unstable; no release cadence.*

## Features

- Forward renderer with per-layer PSO binning across eight render layers (Opaque, Mirrors, Reflected, Transparent, AlphaTested, Shadow, AlphaTestedTreeSprites, OpaqueTessellated).
- Runtime HLSL compilation targeting Shader Model 6.x through DXC, with an on-disk shader cache keyed by source path and entry point.
- Post-processing compute chain: separable Gaussian blur, Sobel edge detection, and an alpha-fixup pass before present.
- Assimp-driven model import through a thin wrapper (`Common/MyImporter.h`).
- In-engine ImGui toolchain on the docking branch, including a live descriptor-heap visualizer.
- Explicit resource state-transition batching through `BarrierManager`.
- Stable, predictable GPU descriptor indices through `DescHeapManager`.

## Build and run

Prerequisites:

- Visual Studio 2022 with the *Desktop development with C++* workload (platform toolset v143).
- Windows 10 SDK 10.0 or newer (whichever ships with VS2022 is sufficient).
- A Direct3D 12-capable GPU.

Build from the command line:

```
msbuild MC_Engine\MC_Engine.sln /p:Configuration=Release /p:Platform=x64
```

Or open `MC_Engine\MC_Engine.sln` in Visual Studio and build `x64 | Release`.

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
│   ├── Includes/                # Vendored headers: Assimp, DXC, ImGui
│   ├── Libs/                    # Vendored static libs
│   ├── dll files/               # Runtime DLLs copied next to the EXE
│   ├── packages/                # NuGet packages (WinPixEventRuntime)
│   ├── MCEngine.cpp / .h        # Engine class: renderer state, frame resources, render items
│   ├── MC_PipelineManager.cpp   # PSO and root signature construction
│   ├── ForwardPass.cpp          # Main render loop; layer iteration and draw submission
│   ├── DescHeapManager.cpp / .h # Descriptor heap allocator
│   ├── BarrierManager.cpp / .h  # Resource state-transition batching
│   ├── Scene_Ch7.cpp / .h       # Original Luna Chapter 7 scene
│   ├── Scene_grass.cpp / .h     # Instanced grass scene with culling
│   ├── Scene_Empty.cpp / .h     # Minimal scene for baseline measurements
│   ├── ShaderLib*.cpp           # Runtime HLSL compilation cache (DXC)
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

### Post-processing compute chain

Three compute shaders run after the forward pass:

- `Assets/Shaders/blursCS.hlsl` — separable Gaussian blur. Horizontal and vertical passes; sigma and iteration count exposed to the UI.
- `Assets/Shaders/SobelCS.hlsl` — 3×3 Sobel edge detection over the blurred result.
- `Assets/Shaders/ForceAlpha.hlsl` — alpha-channel fixup for the final composite.

Each stage writes to a UAV; the final stage writes the swap-chain back buffer before present.

## Roadmap

Tracked items, in priority order:

1. Resource wrapper for persistent GPU buffers.
2. Resource lifetime and barrier abstraction — subsumes the current `BarrierManager`.
3. Descriptor heap optimization (multi-heap, ring segments, retirement).
4. Frame graph / render graph.
5. Scene save and load.

## References

- Frank Luna, *Introduction to 3D Game Programming with DirectX 12* — starting-point sample structure.
- Microsoft **MiniEngine** (DirectX-Graphics-Samples) — resource management patterns.
- Microsoft **DirectX-Graphics-Samples** — DXC usage and bindless patterns.
- Jason Gregory, *Game Engine Architecture* — subsystem decomposition.
- Fabian Giesen, *The ryg blog* — GPU-side reasoning and micro-architecture intuition.

## License

MIT. See `LICENSE`.
