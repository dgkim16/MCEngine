# How MCEngine::GrassCullDispatch() Works

`GrassCullDispatch()` is called once per frame (before the forward pass) when GPU culling is active.
Its job is to take all grass instances, run the cull shader on the GPU, and leave a compact
`gVisible` buffer and a ready-to-use `ExecuteIndirect` argument buffer for the draw call.

---

## Early Exit

```cpp
if (!mGrassScene || !mGrassScene->useGpuCulling) return;
```

The function is a no-op unless a grass scene is registered and the GPU culling toggle is on.
All the resources below live inside `Scene_grass`; without a scene pointer they don't exist.

---

## Step 1 — Reset the Counter

```cpp
mCommandList->CopyBufferRegion(
    gs->mGrassCounterBuffer.Get(), 0,
    gs->mGrassCounterResetBuffer.Get(), 0,
    sizeof(UINT));
```

`mGrassCounterBuffer` holds a single `uint` that the cull shader increments atomically each
time a blade passes culling. Before dispatching the shader, that counter must be zeroed so the
new frame starts counting from 0.

`CopyBufferRegion` is used instead of a UAV clear because the source
(`mGrassCounterResetBuffer`) is a permanently-mapped upload heap buffer that always contains the
value `0`. Copying 4 bytes from it is the cheapest zero-write available on the GPU. A full
`ClearUnorderedAccessViewUint` would require a heap-resident descriptor, which adds overhead.

At the point this copy runs, `mGrassCounterBuffer` is in `COPY_DEST` (where it was left at
the end of the previous frame's dispatch) and `mGrassCounterResetBuffer` is in `GENERIC_READ`
(upload heap resources stay in this state forever).

---

## Step 2 — Transition to UAV

```cpp
CD3DX12_RESOURCE_BARRIER toUAV[] = {
    Transition(mGrassVisibleBuffer,  NON_PIXEL_SHADER_RESOURCE → UNORDERED_ACCESS),
    Transition(mGrassCounterBuffer,  COPY_DEST                 → UNORDERED_ACCESS),
};
```

The GPU's resource state machine requires an explicit barrier any time a resource changes role.

- `mGrassVisibleBuffer` was left as `NON_PIXEL_SHADER_RESOURCE` at the end of the previous
  frame (where it was read by the vertex shader). It must become `UNORDERED_ACCESS` before the
  cull shader can write to it.
- `mGrassCounterBuffer` was in `COPY_DEST` for the reset copy above. The cull shader's
  `InterlockedAdd` is a UAV operation, so it also needs `UNORDERED_ACCESS`.

Both transitions are issued in a single `ResourceBarrier` call so the driver can batch them.

---

## Step 3 — Upload the Constant Buffer

```cpp
GrassCullCB cullCB = {};
XMMATRIX vpT = XMLoadFloat4x4(&mMainPassCB.ViewProj);
// vpT.r[i] = column i of the mathematical ViewProj (stored transposed)
XMVECTOR rawPlanes[6] = {
    vpT.r[3] + vpT.r[0],   // left
    vpT.r[3] - vpT.r[0],   // right
    vpT.r[3] + vpT.r[1],   // bottom
    vpT.r[3] - vpT.r[1],   // top
    vpT.r[2],               // near
    vpT.r[3] - vpT.r[2],   // far
};
for (int i = 0; i < 6; i++)
    XMStoreFloat4(&cullCB.FrustumPlanes[i], XMPlaneNormalize(rawPlanes[i]));
cullCB.EyePosW            = mMainPassCB.EyePosW;
cullCB.DrawDistance       = 500.0f;
cullCB.InstanceCount      = gs->mTotalGrassInstances;
cullCB.GrassMaterialIndex = gs->mGrassMaterial->MatCBIndex;
cullCB.SphereRadius       = gs->grassHeight;
gs->mGrassCullCB->CopyData(0, cullCB);
```

**Frustum planes (Gribb-Hartmann extraction):**
`mMainPassCB.ViewProj` stores `Transpose(M_vp)` — the mathematical view-projection matrix
transposed for HLSL's column-major convention. Loading it into `XMMATRIX` gives a matrix
whose *rows* equal the *columns* of the original `M_vp`. Gribb-Hartmann for DirectX's
row-vector convention says the frustum planes come from the columns of `M_vp`, so
`vpT.r[i]` is exactly what we need. Six plane vectors are built by adding/subtracting
pairs of rows, then normalised with `XMPlaneNormalize` so the shader can use a simple
signed-distance threshold without dividing by plane length.

**`GrassMaterialIndex`:**
The static instance buffer (`mGrassFullInstanceBuffer`) was uploaded during `Load()`, before
`SwitchScene` reassigns `MatCBIndex` values. Uploading the current `MatCBIndex` here lets
the shader override the potentially-stale value stored in each `InstanceData`.

**`SphereRadius`:**
The shader performs a bounding-sphere frustum test. The sphere encloses the full blade height,
so passing `grassHeight` as the radius ensures no visible blade is culled.

**`CopyData`:**
`mGrassCullCB` is a `UploadBuffer<GrassCullCB>` — a persistently-mapped upload heap
resource. `CopyData` just does a `memcpy` into the mapped pointer. The GPU reads it
directly via its virtual address passed to `SetComputeRootConstantBufferView`.

---

## Step 4 — Dispatch the Cull Shader

```cpp
mCommandList->SetComputeRootSignature(mGrassCullRootSignature.Get());
mCommandList->SetPipelineState(mPSOs["grass_cull_cs"].Get());
mCommandList->SetComputeRootConstantBufferView(0, mGrassCullCB->Resource()->GetGPUVirtualAddress());
mCommandList->SetComputeRootShaderResourceView(1, mGrassFullInstanceBuffer->GetGPUVirtualAddress());
mCommandList->SetComputeRootUnorderedAccessView(2, mGrassVisibleBuffer->GetGPUVirtualAddress());
mCommandList->SetComputeRootUnorderedAccessView(3, mGrassCounterBuffer->GetGPUVirtualAddress());
UINT groups = (gs->mTotalGrassInstances + 63) / 64;
mCommandList->Dispatch(groups, 1, 1);
```

The root signature for the cull pass has four slots, bound as **root descriptors** (not heap
descriptors). Root descriptors embed the GPU virtual address directly into the root arguments,
avoiding a heap lookup. Only Raw and Structured buffers support root descriptor UAVs; typed
buffers (`RWBuffer<uint>`) do not — which is why `mGrassCounterBuffer` is a
`RWByteAddressBuffer` in the shader.

**Thread group count:**
The cull shader uses `[numthreads(64, 1, 1)]`. The dispatch launches `ceil(N / 64)` groups,
giving exactly enough threads to cover all N instances. The shader's first instruction guards
against out-of-bounds access with `if (id.x >= InstanceCount) return;`.

---

## Step 5 — UAV Barriers

```cpp
CD3DX12_RESOURCE_BARRIER uavBarriers[] = {
    UAV(mGrassVisibleBuffer),
    UAV(mGrassCounterBuffer),
};
```

A UAV barrier stalls subsequent commands until all UAV writes from earlier commands are
globally visible. Without these, the `CopyBufferRegion` in step 7 (which reads the counter)
and the draw call (which reads the visible buffer) could start before the cull shader finishes
writing.

---

## Step 6 — Transition for Reading

```cpp
CD3DX12_RESOURCE_BARRIER postCS[] = {
    Transition(mGrassVisibleBuffer,    UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE),
    Transition(mGrassCounterBuffer,    UNORDERED_ACCESS → COPY_SOURCE),
    Transition(mGrassIndirectArgsBuffer, INDIRECT_ARGUMENT → COPY_DEST),
};
```

Three resources change role simultaneously:

- `mGrassVisibleBuffer` → `NON_PIXEL_SHADER_RESOURCE`: the vertex shader reads the compact
  instance list as a structured buffer (SRV). `NON_PIXEL_SHADER_RESOURCE` covers
  vertex/hull/domain/geometry/compute reads.
- `mGrassCounterBuffer` → `COPY_SOURCE`: the final count is about to be copied into the
  indirect args buffer.
- `mGrassIndirectArgsBuffer` → `COPY_DEST`: it needs to accept the incoming count write.

---

## Step 7 — Patch the Instance Count into the Indirect Args

```cpp
mCommandList->CopyBufferRegion(
    gs->mGrassIndirectArgsBuffer.Get(),
    offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, InstanceCount),
    gs->mGrassCounterBuffer.Get(), 0,
    sizeof(UINT));
```

`D3D12_DRAW_INDEXED_ARGUMENTS` is a five-field struct
(`IndexCountPerInstance`, `InstanceCount`, `StartIndexLocation`, `BaseVertexLocation`,
`StartInstanceLocation`). The static fields were written once in `BuildGpuCullingBuffers`.
Only `InstanceCount` changes every frame — and its value is exactly the counter that the
cull shader just computed. `CopyBufferRegion` copies 4 bytes from byte 0 of the counter
directly into the `InstanceCount` field of the indirect args, using `offsetof` to target
the exact byte offset. The GPU never reads back to the CPU; the entire counter→args
pipeline stays on the GPU timeline.

---

## Step 8 — Restore States for Next Frame

```cpp
CD3DX12_RESOURCE_BARRIER postCopy[] = {
    Transition(mGrassIndirectArgsBuffer, COPY_DEST   → INDIRECT_ARGUMENT),
    Transition(mGrassCounterBuffer,      COPY_SOURCE → COPY_DEST),
};
```

Both resources are returned to the states `GrassCullDispatch` expects at frame start:

- `mGrassIndirectArgsBuffer` → `INDIRECT_ARGUMENT`: ready for `ExecuteIndirect` in
  `ForwardPass`.
- `mGrassCounterBuffer` → `COPY_DEST`: ready for the `CopyBufferRegion` zero-reset at the
  start of the next frame's dispatch.

`mGrassVisibleBuffer` is already in `NON_PIXEL_SHADER_RESOURCE` from step 6, which is also
its expected start-of-frame state.

---

## End-to-End Summary

```
[Frame N start]
  Counter already in COPY_DEST, VisibleBuffer in NON_PIXEL_SHADER_RESOURCE,
  IndirectArgs in INDIRECT_ARGUMENT

GrassCullDispatch():
  1. CopyBufferRegion  → zero the counter       (COPY_DEST source: reset buffer)
  2. Barrier           → counter + visible → UAV
  3. CopyData          → upload CB (planes, distances, radius)
  4. Dispatch          → cull shader writes compact visible list + increments counter
  5. UAV barriers      → wait for CS writes
  6. Barrier           → visible → SRV, counter → COPY_SOURCE, args → COPY_DEST
  7. CopyBufferRegion  → counter value → IndirectArgs.InstanceCount
  8. Barrier           → args → INDIRECT_ARGUMENT, counter → COPY_DEST

ForwardPass():
  ExecuteIndirect(mGrassCommandSignature, mGrassIndirectArgsBuffer)
    → draws exactly N visible instances from mGrassVisibleBuffer
```
