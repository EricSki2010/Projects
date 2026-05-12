# Notes

This is purely a test, the files are in disorder, just a prototype.

# PerlinNoiseVisualizer Architecture

How the 3D voxel world is generated, meshed, and uploaded across multiple threads. The `"noise2D"` scene is trivial in comparison — one quad built once in `onEnter` — and isn't covered here. See [api_reference.md](api_reference.md) for its entry point.

## High-Level Flow

The voxel world is unbounded in X/Z and bounded in Y (`Y_MIN_WORLD = -100`, `Y_MAX_WORLD = 251`). Chunks within view distance of the camera stream in continuously; chunks beyond `KEEP_DIST` (1.5× view distance) are evicted. The pipeline that produces a renderable chunk has **three stages**, each on its own thread pool or thread:

```
   discovery (main)
        │
        ▼
  ┌───────────────┐    ┌───────────────┐    ┌───────────────┐
  │  Stage 1      │    │  Stage 2      │    │  Stage 3      │
  │  Perlin       │───▶│  Greedy mesh  │───▶│  GL upload    │
  │  (worker pool)│    │  (worker pool)│    │  (main thread)│
  └───────────────┘    └───────────────┘    └───────────────┘
        │                    │                    │
        ▼                    ▼                    ▼
  sWorkerVoxels       sMeshUploadQueue      ctx.mergedMeshes
  (shared map)        (locked queue)        (engine state)
```

The engine's auto-rebuild is suppressed throughout (`ctx.needsRebuild = false` every frame). The scene drives `ctx.mergedMeshes` directly via per-chunk index bookkeeping.

## Worker Pool Sizing
**Function:** [scene.cpp:605 — startWorkers](../scene.cpp#L605)

```
total       = std::thread::hardware_concurrency()
gen budget  = max(2, total - RESERVED_FOR_SYSTEMS)    // RESERVED_FOR_SYSTEMS = 2
n2 (Stage 2)= max(1, gen / 4)                          // ~25% do greedy meshing
n1 (Stage 1)= max(1, gen - n2)                         // ~75% do Perlin sampling
```

`RESERVED_FOR_SYSTEMS = 2` leaves headroom on the OS for the main thread plus future game systems (entities, physics, AI). Lower it for more gen throughput at the cost of frame stability.

## Stage 1 — Perlin Sampling
**Function:** [scene.cpp:168 — computeChunkVoxels](../scene.cpp#L168)

Workers pop a chunk coord from `sStage1Queue` and produce a fully-formed `WorkerVoxelChunk` (a flat `kChunkVoxelCount`-byte array). Two distinct fill regions, split at `Y_TOP_RAMP_START = 200`:

**Below `Y_TOP_RAMP_START`:** 3D-noise terrain with a y-based density bias:
- `y ∈ [0, 200)` — linear ramp from `HEIGHT_MOD_BOTTOM = -0.4` (dense) to `HEIGHT_MOD_TOP = +0.3` (sparser/cavier)
- `y ∈ [Y_RAMP_START = -90, 0)` — capped at `HEIGHT_MOD_BOTTOM`
- `y ∈ [-100, -90)` — 10-block fade to bias `+1.0`, forcing an empty bedrock void
- A voxel is placed if `sample3D(x, y, z) ≤ NOISE_THRESHOLD + bias`

**Above `Y_TOP_RAMP_START`:** Per-column 2D heightmap. For each `(x, z)` column we compute `surfaceY = 200 + noise01 * amp`, where `amp` is the biome-blended top-ramp depth (see below). Cells at or below `surfaceY` get filled, with a 3D-noise carve whose bias ramps from `HEIGHT_MOD_TOP` at y=200 to `HEIGHT_MOD_MAX = +0.6` at y=`Y_HEIGHT_MAX_END = 240` (and stays there). The carve pass only **removes** blocks — it never adds.

### Biome IDW Amplitude Blend
The per-column amplitude `amp` is the trickiest part of Stage 1.

For each chunk we first run a **9-point uniform-biome short-circuit**: sample at the 4 extended-padding corners, the 4 chunk corners, and the chunk center. If all 9 agree, the chunk is uniform and we skip the rest of the pass, reusing one amplitude. (4-corner alone was wrong: thin biome bands could slip between the extended corners undetected and produce snap-step amplitude discontinuities.)

Otherwise we:
1. Fill a `(kChunkSize + 2P)²` biome cache around the chunk via `getCachedBiome` — `P = B + S = 5 + 10 = 15`.
2. Compute an "interior" flag for each cell in the `(kChunkSize + 2S)²` region — a cell is interior if every neighbor within `BIOME_INTERIOR_RADIUS = 5` is the same biome.
3. For each column inside the chunk: if it's interior, use its own biome's `biomeTopRampDepth`. Otherwise, search outward up to `BIOME_SEARCH_RADIUS = 10` for up to **3** distinct interior-biome columns and IDW-blend their amplitudes by `1 / max(dist², 1)`.

This produces a per-column amplitude that smoothly transitions across biome boundaries instead of stepping at the threshold.

### Dispatch Tiering
**Function:** [scene.cpp:367 — stage1WorkerLoop](../scene.cpp#L367)

Workers don't just FIFO the queue — they prefer chunks at specific Y tiers. Worker N prefers `playerCY + tierToYOffset(N % PRIMARY_Y_TIERS)` where the offsets are `{0, -1, +1, -2, +2}`. Workers 0–4 each cover a unique tier; workers 5+ double up starting at the player's tier and working outward. The main thread's discovery loop applies the same tier-scored selection plus a forward-direction bias (chunks in front of the camera score lower than chunks beside or behind).

## Stage 2 — Greedy Meshing
**Function:** [scene.cpp:442 — greedyMeshFromSnapshot](../scene.cpp#L442)

When a Stage 1 worker finishes, it pushes itself plus all 6 neighbors onto `sStage2Queue` — the boundary faces of every neighbor may need re-evaluation now that this chunk's voxels exist.

A Stage 2 worker pops a chunk, takes a `shared_lock` on `sWorkerVoxels`, captures raw pointers to self + up to 6 neighbors into a `VoxelSnapshot`, and runs the greedy mesher under that lock. Stage 1 only **inserts** new entries (never modifies existing ones), so the captured pointers stay valid for the lifetime of the lock.

For each solid voxel, each of its 6 faces is checked: if the neighbor in that direction is empty (or in a missing chunk — treated as empty for boundary), a quad is emitted, then `VEGreedy::greedyMeshPlane` merges co-planar quads.

A `stage2Done` flag is set **only** when all 6 neighbors were present at meshing time. Future pushes for that chunk are skipped — its mesh is "final" and re-meshing would just orphan arena bytes for identical output.

The output `MeshUpload` (interleaved verts at 8 floats/vertex + indices) goes onto `sMeshUploadQueue`.

## Stage 3 — GL Upload (Main Thread)
**Function:** [scene.cpp:723 — onUpdate](../scene.cpp#L723)

`onUpdate` drains `sMeshUploadQueue` under a time budget:
- `BUILD_BUDGET_MS = 8.0` per frame (`MAX_CHUNKS_PER_FRAME = 256` soft cap)
- For each upload, constructs a `Mesh` (the only step that touches GL), packages it into a `MergedMeshEntry`, and splices it into `ctx.mergedMeshes`.
- A `sChunkToMergedIdx` map maps chunk coord → vector index, and a `sFreeMergedSlots` stack tracks indices freed by evictions. New entries reuse a free slot if one exists, otherwise `push_back` — this keeps `ctx.mergedMeshes` bounded by the live-chunk count even after long exploration.
- Empty greedy results (fully-empty or fully-occluded chunks) clear any prior entry without uploading anything new.

## Eviction

Two independent eviction paths keep memory bounded:

**Voxel eviction** ([scene.cpp:824](../scene.cpp#L824)) — Once a chunk's 6-neighbor ring is all built, its 4 KB voxel array is no longer needed (greedy won't re-read it). We delay the erase by `EVICT_DELAY_SECONDS = 2.0` to cover any in-flight Stage 2 worker still holding a snapshot pointer.

**Distance-based chunk eviction** ([scene.cpp:840](../scene.cpp#L840)) — Anything past `KEEP_DIST = VIEW_DIST * 1.5` from the camera drops from every container: `sBuilt`, `sWorkerVoxels`, `sChunkToMergedIdx`, and the freed merged slot is pushed onto `sFreeMergedSlots`. World-boundary chunks (at Y extremes) can't hit the 6-neighbor eviction path because some neighbors are forever missing — distance is the only thing that cleans them up.

Stale pending chunks (queued by past discovery but now past `KEEP_DIST`) are also swept from `sPendingChunks` / `sPendingSet` each frame; without this they leaked proportional to total camera travel.

## Constants Reference

| Constant | Value | Purpose |
| --- | --- | --- |
| `GRID` | 100 | Cosmetic starting-cam reference |
| `BIOME_INTERIOR_RADIUS` (B) | 5 | "Solid biome" definition for IDW |
| `BIOME_SEARCH_RADIUS` (S) | 10 | Max distance to find interior neighbors |
| `HEIGHTMAP_FREQ_MULT` | 0.05 | Per-column surface noise frequency |
| `NOISE_THRESHOLD` | 0.15 | Base 3D-noise solid-fill cutoff |
| `HEIGHT_MOD_BOTTOM` | -0.4 | Density bias at y=0 (dense) |
| `HEIGHT_MOD_TOP` | +0.3 | Density bias at y=200 (sparser) |
| `HEIGHT_MOD_MAX` | +0.6 | Surface-carve bias at y≥240 |
| `Y_MIN_WORLD` / `Y_MAX_WORLD` | -100 / 251 | World vertical bounds (exclusive top) |
| `Y_RAMP_START` | -90 | Last fully-capped layer (bedrock fade starts here) |
| `Y_TOP_RAMP_START` | 200 | Last 3D-noise layer (heightmap region starts here) |
| `Y_HEIGHT_MAX_END` | 240 | Surface-carve bias reaches `HEIGHT_MOD_MAX` |
| `BUILD_BUDGET_MS` | 8.0 | Per-frame GL-upload budget |
| `MAX_CHUNKS_PER_FRAME` | 256 | Soft safety cap |
| `VIEW_DIST` | 344.0 | Render distance (== `FOG_END`) |
| `KEEP_DIST` | `VIEW_DIST * 1.5` | Eviction threshold |
| `EVICT_DELAY_SECONDS` | 2.0 | Voxel-array hold after 6-neighbor ring built |
| `PRIMARY_Y_TIERS` | 5 | Unique Y-offset tiers worker pool covers |
| `RESERVED_FOR_SYSTEMS` | 2 | Logical threads withheld from gen pool |
| `BIOME_CACHE_MAX_ENTRIES` | 500,000 | Hard cap on shared biome cache (~30 MB) |
| `BENCHMARK_CHUNK_COUNT` | 100 | Chunks per `[bench]` log line |

## Benchmark Output

Every `BENCHMARK_CHUNK_COUNT` Stage-3 completions, scene.cpp emits a `[bench]` line with elapsed ms plus every potentially-growing container size: `merged`, `freeSlots`, `live`, `built`, `pending`, `pendSet`, `inflight`, `voxels`, `evictQ`, `biome`, `s1Q`, `s2Q`, `upQ`. If any of those keep climbing across batches while standing still or flying in circles, that container has a leak.
