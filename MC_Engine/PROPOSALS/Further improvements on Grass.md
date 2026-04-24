# 1. Remove globallycoherent from gVisible (free, immediate)

globallycoherent forces a global cache flush on every write. It's only required on buffers that other
thread groups read during the same dispatch. gVisible is write-only — nothing reads it until after
the dispatch ends. Drop it:

RWStructuredBuffer<GrassInstanceData> gVisible : register(u0);

gVisibleCount still needs it because InterlockedAdd must be globally visible across thread groups.

---
# 2. Wave intrinsics to batch the global atomic (big win)

The current code does one InterlockedAdd per visible thread — potentially thousands of global atomic
operations, each contending on the same counter. Wave intrinsics let an entire wave (32–64 threads)
reserve its slots with a single atomic:
```c
[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    bool visible = false;
    GrassInstanceData inst;

    if (id.x < InstanceCount)
    {
        inst = gAllInstances[id.x];
        float3 centerW = float3(inst.World._41, inst.World._42, inst.World._43);

        float3 toEye = centerW - EyePosW;
        if (dot(toEye, toEye) < DrawDistance * DrawDistance)
        {
            visible = true;
            [unroll]
            for (int i = 0; i < 6; i++)
            {
                if (dot(FrustumPlanes[i].xyz, centerW) + FrustumPlanes[i].w < -SphereRadius)
                {
                    visible = false;
                    break;
                }
            }
        }
    }

    // One InterlockedAdd per wave instead of one per visible thread
    uint waveVisible  = WaveActiveCountBits(visible);
    uint wavePrefixOff = WavePrefixCountBits(visible);
    uint waveBase = 0;
    if (WaveIsFirstLane())
        gVisibleCount.InterlockedAdd(0, waveVisible, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);

    if (visible)
        gVisible[waveBase + wavePrefixOff] = inst;
}
```
This reduces global atomics from N_visible to ceil(N_visible / WaveSize) — typically 32–64× fewer on
NVIDIA/AMD.

Two small bonuses included above:
- Squared distance (dot(toEye, toEye) < DrawDistance²) removes the sqrt from distance()
- [unroll] on the 6-plane loop avoids loop counter overhead

---
# 3. Separate position buffer for culling reads (architectural, larger gain at scale)

Right now every thread loads 64 bytes (full float4x4) to extract only 12 bytes (_41/_42/_43). With a
parallel StructuredBuffer<float4> gPositions containing just the blade translations (16 bytes each),
8 instances fit per cache line instead of 2. For large grass fields the culling read becomes ~4× more
cache-efficient, and only visible instances pay the cost of loading the full GrassInstanceData.

That requires maintaining a second upload buffer alongside mGrassFullInstanceBuffer, so it's more
invasive, but at 200×200+ blades it would matter most.

---
Priority order: globallycoherent removal is zero-cost. Wave intrinsics is the highest-leverage change
for the atomic bottleneck. Squared distance + [unroll] are minor free wins. The position buffer is
worth it at large counts.