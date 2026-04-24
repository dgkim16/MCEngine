# How Scene_grass::BuildGpuCullingBuffers() Works

`BuildGpuCullingBuffers()` is called once during `Scene_grass::Load()`. It allocates and
initialises every GPU resource that the GPU culling pipeline needs. None of these resources
exist when the scene is first loaded, so the function starts by resetting them all to ensure
a clean slate on a hot reload.

---

## Resource 1 — `mGrassFullInstanceBuffer` (+ `mGrassFullInstanceUpload`)

```cpp
mGrassFullInstanceBuffer = d3dUtil::CreateDefaultBuffer(
    device, cmdList, transposed.data(), instBufSize, mGrassFullInstanceUpload);
```

**What it is:** A default-heap (GPU-only) structured buffer containing one `InstanceData`
entry per grass blade — world matrix, texture transform, and material index. It is written
exactly once (here, at load time) and read every frame by the cull shader as a read-only SRV.

**Why default heap, not upload heap:** Upload heap resources live in write-combined CPU
memory. Reads from the GPU are slow on write-combined memory. Default heap resources reside
in fast VRAM. Because this buffer is read thousands of times per frame (once per blade per
dispatch) and never written again after upload, default heap is the right choice.

**Why the matrices are transposed before upload:**
`UpdateInstanceData()` — the CPU-side instancing path — calls `XMMatrixTranspose` on every
world matrix before writing it to the per-frame upload buffer. This is because HLSL treats
`float4x4` as column-major: when C++ writes a row-major `XMFLOAT4X4` directly, HLSL would
see the matrix transposed. The transpose before upload cancels this out, restoring the correct
mathematical matrix. The GPU cull shader extracts the blade's world position using
`inst.World._41/_42/_43` (the fourth row's xyz). For a transposed matrix, `_41/_42/_43` is
the translation column — the correct world-space position. Without the transpose, those
components would be zero.

**Why `mGrassFullInstanceUpload` is kept as a member:**
`CreateDefaultBuffer` records a `CopyBufferRegion` command on the command list but does not
execute it immediately. The upload buffer must stay alive until the command list is executed
and the GPU has finished the copy — otherwise the source data disappears before the copy
completes. It is safe to release it after the subsequent `FlushCommandQueue` in
`BuildGpuCullingBuffers`... but it is simpler and safe to just keep it as a member and release
it in `Deactivate`.

---

## Resource 2 — `mGrassVisibleBuffer`

```cpp
auto desc = CD3DX12_RESOURCE_DESC::Buffer(instBufSize,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
device->CreateCommittedResource(&heapProps, ..., D3D12_RESOURCE_STATE_COMMON, ...);
```

**What it is:** A default-heap buffer sized to hold *all* `mTotalGrassInstances` entries —
the same element type and size as `mGrassFullInstanceBuffer`. Each frame the cull shader
writes a compact subset of surviving blades into this buffer. The draw call reads it as an
SRV to fetch per-instance data in the vertex shader.

**Why it is sized for the maximum (all instances):** The cull result is
non-deterministic at allocation time. In the worst case (camera inside the grass patch,
all blades visible), every instance survives culling. The buffer must be large enough for
that case.

**Why `ALLOW_UNORDERED_ACCESS`:** The cull shader writes to it via `RWStructuredBuffer`
(a UAV). D3D12 requires this flag on any resource that will be bound as a UAV. The flag
cannot be added later.

**Why it starts in `COMMON`:** `CreateCommittedResource` places the resource in whatever
initial state you specify. `COMMON` is valid for a default-heap buffer that has no data yet.
A later barrier (in the same `Load` command list) transitions it to
`NON_PIXEL_SHADER_RESOURCE`, which is the state `GrassCullDispatch` expects at the start of
every frame.

---

## Resource 3 — `mGrassCounterBuffer`

```cpp
auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT),
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
```

**What it is:** A single `uint` (4 bytes) on the default heap. The cull shader calls
`gVisibleCount.InterlockedAdd(0, 1, slot)` on it: an atomic fetch-and-increment that gives
each surviving blade a unique write slot. After the dispatch, the value equals the number of
blades that passed culling — the visible instance count for this frame.

**Why it must be on the default heap and not an upload buffer:**
`InterlockedAdd` is a UAV atomic operation. UAV atomics require `ALLOW_UNORDERED_ACCESS`,
which is only legal on default-heap resources. Upload heap resources (`D3D12_HEAP_TYPE_UPLOAD`)
cannot have UAV access.

**Why only 4 bytes:**
One `uint` is all that is needed. The shader only ever increments it and reads its value
back (via `CopyBufferRegion`) to populate `IndirectArgs.InstanceCount`. There is no need
for a larger counter array.

**State lifecycle per frame:**
```
Start of frame:  COPY_DEST       (ready for zero-reset)
After reset:     COPY_DEST       (unchanged; CopyBufferRegion writes to it)
Before dispatch: UNORDERED_ACCESS (shader atomics)
After dispatch:  COPY_SOURCE     (GPU reads it to patch IndirectArgs)
End of frame:    COPY_DEST       (left here ready for next frame's reset)
```

---

## Resource 4 — `mGrassCounterResetBuffer`

```cpp
CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
// ...
UINT* mapped = nullptr;
mGrassCounterResetBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
*mapped = 0u;
mGrassCounterResetBuffer->Unmap(0, nullptr);
```

**What it is:** A 4-byte upload-heap buffer that permanently holds the value `0`. It is
the source for the per-frame `CopyBufferRegion` that zeros `mGrassCounterBuffer`.

**Why a dedicated buffer instead of clearing the counter another way:**
- `ClearUnorderedAccessViewUint` requires a CPU-visible descriptor handle, which means a
  heap allocation and descriptor management.
- Writing `0` via a mapped pointer on the CPU and waiting is a CPU-GPU sync stall.
- `CopyBufferRegion` from an upload resource is the lightest GPU-side zero-write available:
  it runs on the copy engine, takes no descriptor, and has no sync cost.
The upload heap buffer can be persistently mapped because upload heap resources may remain
mapped for their entire lifetime.

**Why it is never modified after creation:**
Its entire purpose is to be a static source of `0`. Writing anything else to it would break
the counter reset every frame.

---

## Resource 5 — `mGrassIndirectArgsBuffer` (+ `mGrassIndirectArgsUpload`)

```cpp
D3D12_DRAW_INDEXED_ARGUMENTS initArgs = {};
initArgs.IndexCountPerInstance = mGrassRitem->IndexCount;
initArgs.InstanceCount         = 0;
initArgs.StartIndexLocation    = mGrassRitem->StartIndexLocation;
initArgs.BaseVertexLocation    = mGrassRitem->BaseVertexLocation;
initArgs.StartInstanceLocation = 0;

mGrassIndirectArgsBuffer = d3dUtil::CreateDefaultBuffer(
    device, cmdList, &initArgs, sizeof(initArgs), mGrassIndirectArgsUpload);
```

**What it is:** A default-heap buffer containing one `D3D12_DRAW_INDEXED_ARGUMENTS` struct
(20 bytes). `ExecuteIndirect` reads this buffer to issue a `DrawIndexedInstanced` call
without the CPU ever knowing how many instances to draw.

**Why the static fields are written at load time:**
`IndexCountPerInstance`, `StartIndexLocation`, `BaseVertexLocation`, and
`StartInstanceLocation` are fixed properties of the grass mesh. They never change. Writing
them here means the per-frame `CopyBufferRegion` only needs to patch the single
`InstanceCount` field (4 bytes) instead of rebuilding all 20 bytes every frame.

**`InstanceCount = 0` at init:**
The first frame before any dispatch would read a garbage value if it were left uninitialised.
Zero is safe: `ExecuteIndirect` with `InstanceCount = 0` draws nothing.

**Why `mGrassIndirectArgsUpload` is kept as a member:**
Same reason as `mGrassFullInstanceUpload`: `CreateDefaultBuffer` schedules the copy on the
command list; the upload buffer must remain alive until the GPU has consumed it after the
subsequent `FlushCommandQueue`.

**State lifecycle per frame:**
```
Start of frame:  INDIRECT_ARGUMENT (ExecuteIndirect reads it)
Before counter copy: COPY_DEST    (ready to receive the new InstanceCount)
After counter copy:  INDIRECT_ARGUMENT (restored, ready for draw)
```

---

## Resource 6 — `mGrassCullCB`

```cpp
mGrassCullCB = std::make_unique<UploadBuffer<GrassCullCB>>(device, 1, false);
```

**What it is:** A `UploadBuffer` wrapping a single `GrassCullCB` element. `UploadBuffer`
creates a persistently-mapped upload-heap resource and exposes `CopyData(index, data)` as a
typed `memcpy` into it.

**Why upload heap (not default heap):**
The CB is written by the CPU every frame (`GrassCullDispatch` calls `CopyData` before each
dispatch) with fresh frustum planes, eye position, and draw distance. Upload heap resources
are mapped write-combined memory — fast for CPU writes, directly readable by the GPU via
virtual address. A default heap resource would require a staging buffer and a copy command
just to update 128 bytes, which is wasteful for data that changes every frame.

**Why `false` (not constant buffer aligned):**
`UploadBuffer`'s boolean argument controls whether elements are padded to 256-byte CB
alignment. `false` means tight packing. The CB is bound via
`SetComputeRootConstantBufferView` which passes the raw GPU virtual address; the hardware
reads exactly `sizeof(GrassCullCB)` bytes from that address, so alignment padding is not
needed here.

**Why one element, not three (one per frame resource):**
Frame resources exist to avoid CPU-GPU conflicts: if the CPU writes into a buffer while the
GPU is reading it from the previous frame, data corruption occurs. `GrassCullDispatch` runs
inside the frame's command list, which is recorded and submitted in order. By the time
`CopyData` is called, the previous frame's GPU work referencing this CB has already
completed (because `FlushCommandQueue`-style synchronisation ensures frame N's GPU work
finishes before frame N+1's CPU work). A single element is therefore safe.

---

## The Initial State Barriers

```cpp
CD3DX12_RESOURCE_BARRIER initBarriers[] = {
    Transition(mGrassVisibleBuffer,      COMMON          → NON_PIXEL_SHADER_RESOURCE),
    Transition(mGrassCounterBuffer,      COMMON          → COPY_DEST),
    Transition(mGrassIndirectArgsBuffer, GENERIC_READ    → INDIRECT_ARGUMENT),
};
cmdList->ResourceBarrier(_countof(initBarriers), initBarriers);
```

These barriers place each resource in the state that `GrassCullDispatch` expects to find it
at the start of every frame:

- `mGrassVisibleBuffer` → `NON_PIXEL_SHADER_RESOURCE`: the first thing `GrassCullDispatch`
  does is transition it *from* this state to `UNORDERED_ACCESS`. If it started in `COMMON`,
  the D3D12 validation layer would complain about an invalid source state.
- `mGrassCounterBuffer` → `COPY_DEST`: the first operation in `GrassCullDispatch` is a
  `CopyBufferRegion` *into* this buffer (the zero-reset). `COPY_DEST` is required for a
  copy destination.
- `mGrassIndirectArgsBuffer` → `INDIRECT_ARGUMENT`: `CreateDefaultBuffer` leaves resources
  in `GENERIC_READ` (the only state upload copies can leave a resource in). `INDIRECT_ARGUMENT`
  is the required state for `ExecuteIndirect`.

`mGrassFullInstanceBuffer` and `mGrassCounterResetBuffer` need no transition here:
`CreateDefaultBuffer` leaves default-heap resources in `GENERIC_READ`, which includes
`NON_PIXEL_SHADER_RESOURCE` — the state the cull shader expects when reading it as an SRV.
`mGrassCounterResetBuffer` is an upload-heap resource; upload-heap resources are always in
`GENERIC_READ` and cannot be transitioned.
