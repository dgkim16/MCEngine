# How GrassCull.hlsl Works

The shader runs one thread per grass blade. Each thread independently decides whether its blade
survives culling, and if so, writes it into a compact output array.

---
# The Three Buffers
```
StructuredBuffer<InstanceData>                    gAllInstances : register(t0);
globallycoherent RWStructuredBuffer<InstanceData> gVisible      : register(u0);
globallycoherent RWByteAddressBuffer               gVisibleCount : register(u1);
```
### ```StructuredBuffer<InstanceData>``` 
— read-only input. A typed array where every element is exactly sizeof(InstanceData) bytes. The GPU knows the stride at compile time, so gAllInstances[i] directly indexes element i.

### ```RWStructuredBuffer<InstanceData>``` 
— read/write version of the same thing. Elements are still sizeof(InstanceData) apart; you can write gVisible[i] = inst.

### ```RWByteAddressBuffer``` 
— a raw, untyped blob of bytes. It has no concept of element size. You address it in bytes (```Load(byteOffset)```, ```Store(byteOffset, value)```). The only reason it exists here instead of a RWBuffer<uint> is a D3D12 root descriptor restriction: only Raw and Structured buffers can be bound directly via ```SetComputeRootUnorderedAccessView```. Typed buffers (RWBuffer<uint>) require a heap descriptor. ```RWByteAddressBuffer``` is the "raw" variant that sidesteps this.

### ```globallycoherent``` 
— by default, UAV writes in a compute shader are only guaranteed visible to other
threads in the same thread group. ```globallycoherent``` promotes that guarantee to all thread groups
across the entire dispatch. Without it, InterlockedAdd and the subsequent gVisible[slot] write could
produce race conditions visible to threads in other groups.

---
### ```InterlockedAdd``` — the Atomic Counter
```
uint slot;
gVisibleCount.InterlockedAdd(0, 1, slot);

InterlockedAdd(byteOffset, value, originalValue) atomically does:
originalValue = *byteOffset;
*byteOffset  += value;
```
in a single indivisible GPU operation — no two threads can interleave their read-modify-write.

- 0 — byte offset into gVisibleCount. The entire buffer is just one uint at byte 0.
- 1 — the amount to add.
- slot — receives the value before the increment (the "original value").

So if three threads hit this simultaneously with the counter at 5, they atomically claim slots 5, 6, and 7. The counter ends up at 8. Each thread got a unique, non-overlapping slot. No locking required.

---
### ```gVisible[slot] = inst```
```
gVisible[slot] = inst;
```
The thread uses its claimed slot as an index into gVisible and writes its InstanceData there. Because every surviving thread claimed a different slot via the atomic, no two threads write to the same
index.

---
### Is gVisible contiguous?

Yes — and that's the whole point of the pattern.

Even though N threads out of M total survive the cull tests, the surviving instances are written to indices 0, 1, 2, ..., N-1 with no gaps. The order is non-deterministic (whichever thread wins the atomic first gets the lower slot), but there are no holes. The final counter value in gVisibleCount equals exactly N — which is then copied into the InstanceCount field of the ```D3D12_DRAW_INDEXED_ARGUMENTS``` buffer so ```ExecuteIndirect``` draws exactly N instances.

If the write were ```gVisible[id.x] = inst``` instead (each thread writes to its own fixed index), you'd have a sparse array full of "dead" entries and no way to tell ```ExecuteIndirect``` how many to draw without a separate compaction pass. The atomic counter + slot pattern is the standard GPU stream-compaction idiom that avoids that.