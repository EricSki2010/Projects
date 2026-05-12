#include "scene.h"
#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/EngineGlobals.h"
#include "../VisualEngine/inputManagement/Camera.h"
#include "../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../VisualEngine/renderingManagement/meshing/Greedy.h"
#include "perlinNoiseManagement.h"
#include "biome.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <iostream>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static const int   GRID                 = 100;
static const int   BIOME_INTERIOR_RADIUS = 5;  // columns this far from any other biome are "solid biome"
static const int   BIOME_SEARCH_RADIUS   = 10; // max distance to look for solid neighbors during IDW blend
static const float HEIGHTMAP_FREQ_MULT   = 0.05f; // <1 = larger, smoother hills. Drives per-column surfaceY for y >= Y_TOP_RAMP_START.
// Below Y_TOP_RAMP_START: 3D noise terrain with the original y-bias ramp.
static const float NOISE_THRESHOLD       =  0.15f; // flipped from -0.15 so density mirrors the pre-flip configuration
static const float HEIGHT_MOD_BOTTOM     = -0.4f;  // applied at y = 0   (dense — mostly solid)
static const float HEIGHT_MOD_TOP        =  0.3f;  // applied at y = Y_TOP_RAMP_START - 1 (sparser, more cavey)
static const float HEIGHT_MOD_MAX        =  0.6f;  // surface-carve bias at Y_HEIGHT_MAX_END and above
static const int   Y_HEIGHT_MAX_END      =  240;   // surface bias ramps from HEIGHT_MOD_TOP at y=Y_TOP_RAMP_START to HEIGHT_MOD_MAX at this y
// Main-thread budget for draining mesh-upload results (GL uploads only —
// greedy meshing now runs on workers so this is much cheaper per chunk).
static const double BUILD_BUDGET_MS      = 8.0;
static const int    MAX_CHUNKS_PER_FRAME = 256; // soft safety cap — BUILD_BUDGET_MS is the real limit

static const glm::vec3 FOG_COLOR  = glm::vec3(0.55f, 0.70f, 0.95f);
static const float FOG_START      = 284.0f;
static const float FOG_END        = 344.0f;
static const float VIEW_DIST      = FOG_END;
static const float KEEP_DIST      = VIEW_DIST * 1.5f; // chunks farther than this from the camera get evicted

static const int Y_MIN_WORLD       = -100;
static const int Y_MAX_WORLD       =  251;   // exclusive upper bound — world tops out at y=250 (fits Rock biome's +50 cap above Y_TOP_RAMP_START=200)
static const int Y_RAMP_START      =  -90;   // bottom: last fully-capped layer; below this ramps to bedrock
static const int Y_TOP_RAMP_START  =  200;   // top: last linear-bias layer; above this the heightmap + surface-carve gradient take over

static const int CHUNK_Y_MIN = -((-Y_MIN_WORLD + kChunkSize - 1) / kChunkSize);
static const int CHUNK_Y_MAX = (Y_MAX_WORLD - 1) / kChunkSize;

struct IVec3Hash {
    size_t operator()(const glm::ivec3& v) const {
        return std::hash<int>()(v.x)
             ^ (std::hash<int>()(v.y) << 10)
             ^ (std::hash<int>()(v.z) << 20);
    }
};

// ─────────────────────────────────────────────────────────────────────────
// 3-stage pipeline:
//
//   Stage 1 (worker pool A): chunk coord → voxel array via Perlin sampling.
//                            Writes into sWorkerVoxels (shared, RW-locked).
//   Stage 2 (worker pool B): chunk → greedy mesh from voxel snapshot of self
//                            + 6 neighbors. Pushes vertex/index arrays to
//                            sMeshUploadQueue.
//   Stage 3 (main thread):   pops from sMeshUploadQueue, runs the Mesh ctor
//                            (only the GL upload happens on main), and inserts
//                            or replaces an entry in ctx.mergedMeshes via
//                            sChunkToMergedIdx.
//
// The engine's auto-rebuild is suppressed (ctx.needsRebuild stays false after
// the initial empty rebuild). The scene drives ctx.mergedMeshes directly.
// ─────────────────────────────────────────────────────────────────────────

struct WorkerVoxelChunk {
    std::array<uint8_t, kChunkVoxelCount> cells{};
    std::atomic<bool> stage1Done{false};
    // Set true by Stage 2 once it has built this chunk's mesh with ALL 6
    // neighbors present (so the mesh is "final" — any future push would
    // produce identical output). Subsequent pops for this chunk are skipped,
    // preventing wasteful re-meshing that orphans arena bytes.
    std::atomic<bool> stage2Done{false};
};

// ─── Main-thread state (no synchronization needed; touched only in onUpdate)
static std::vector<glm::ivec3>                    sPendingChunks;
static std::unordered_set<glm::ivec3, IVec3Hash>  sPendingSet;
static std::unordered_set<glm::ivec3, IVec3Hash>  sInFlight;     // dispatched to Stage 1
static std::unordered_set<glm::ivec3, IVec3Hash>  sBuilt;        // Stage 1 complete (per main thread's view)
static std::unordered_map<glm::ivec3, size_t, IVec3Hash> sChunkToMergedIdx; // chunk → ctx.mergedMeshes index
static std::vector<size_t> sFreeMergedSlots;  // indices into ctx.mergedMeshes that are empty and reusable

// ─── Stage 1 plumbing (Perlin) ─────────────────────────────────────────────
static std::deque<glm::ivec3>      sStage1Queue;
static std::mutex                  sStage1Mutex;
static std::condition_variable     sStage1CV;
static std::vector<std::thread>    sStage1Workers;
static std::atomic<int>            sPlayerCY{0};

// ─── Worker-side voxel storage (read by Stage 2 workers, written by Stage 1)
static std::unordered_map<glm::ivec3, std::unique_ptr<WorkerVoxelChunk>, IVec3Hash> sWorkerVoxels;
static std::shared_mutex sWorkerVoxelsMutex;

// ─── Voxel eviction queue. Once a chunk's mesh AND all 6 neighbors' meshes
// are built, its 4 KB voxel array is no longer needed (greedy meshing won't
// re-read it). We delay the actual erase by EVICT_DELAY_SECONDS to safely
// cover any in-flight Stage 2 worker that might still be holding a snapshot
// pointer to those voxels via shared_lock.
struct PendingEviction { glm::ivec3 chunk; double readyAt; };
static std::deque<PendingEviction> sEvictQueue;
static const double EVICT_DELAY_SECONDS = 2.0;

// ─── Stage 2 plumbing (greedy meshing) ─────────────────────────────────────
static std::deque<glm::ivec3>      sStage2Queue;
static std::mutex                  sStage2Mutex;
static std::condition_variable     sStage2CV;
static std::vector<std::thread>    sStage2Workers;

// ─── Stage 3 plumbing (mesh upload to main thread) ─────────────────────────
struct MeshUpload {
    glm::ivec3 chunkCoord;
    std::vector<float>        verts;   // 8 floats per vertex (pos + uv + normal)
    std::vector<unsigned int> indices;
};
static std::queue<MeshUpload>      sMeshUploadQueue;
static std::mutex                  sMeshUploadMutex;

static std::atomic<bool>           sStopFlag{false};

static glm::vec3 chunkCenter(const glm::ivec3& c) {
    return glm::vec3(c) * (float)kChunkSize + glm::vec3(kChunkSize * 0.5f);
}

static int tierToYOffset(int tier) {
    if (tier == 0) return 0;
    if (tier & 1) return -((tier + 1) / 2);
    return tier / 2;
}

// Number of distinct Y-tier preferences before workers wrap around to the
// closest tier again. With PRIMARY_Y_TIERS=5, workers 0..4 each prefer a unique
// tier (offsets {0, -1, +1, -2, +2}); worker 5 doubles up on tier 0 (closest to
// player), worker 6 on tier 1 (2nd closest), and so on. Extra workers thus
// always reinforce where the player actually is, never deeper.
static const int PRIMARY_Y_TIERS = 5;

// Local helper: chunk-local coord (handles negatives) — duplicates the
// engine's chunkLocal because that one is file-static in ChunkMesh.cpp.
static int sceneChunkLocal(int v) {
    int m = v % kChunkSize;
    return m < 0 ? m + kChunkSize : m;
}

static int voxelLocalIdx(int lx, int ly, int lz) {
    return (lz * kChunkSize + ly) * kChunkSize + lx;
}

// ─────────────────────────────────────────────────────────────────────────
// Stage 1: Perlin sampling. Worker writes a fully-formed WorkerVoxelChunk.
// ─────────────────────────────────────────────────────────────────────────
static std::unique_ptr<WorkerVoxelChunk> computeChunkVoxels(const glm::ivec3& chunk) {
    auto out = std::make_unique<WorkerVoxelChunk>();
    int x0 = chunk.x * kChunkSize, y0 = chunk.y * kChunkSize, z0 = chunk.z * kChunkSize;
    int x1 = x0 + kChunkSize;
    int z1 = z0 + kChunkSize;
    int yLo = std::max(y0, Y_MIN_WORLD);
    int yHi = std::min(y0 + kChunkSize, Y_MAX_WORLD);
    const float bottomRampDepth = (float)(Y_RAMP_START - Y_MIN_WORLD); // = 10, used by the bedrock fade

    // ── Biome IDW pass: per-column blended surface amplitude. ───────────────
    // Padding past the chunk: B (interior radius) + S (search radius) so we can
    // determine interior status for any cell within S of the chunk edge.
    const int B = BIOME_INTERIOR_RADIUS;
    const int S = BIOME_SEARCH_RADIUS;
    const int P = B + S;
    const int E = kChunkSize + 2 * P;        // extended biome cache size
    const int I = kChunkSize + 2 * S;        // interior-flag grid size

    // 9-point uniform-biome short-circuit: sample biome at the 4 *extended*
    // corners (catches boundaries running outside the chunk's own footprint),
    // the 4 *chunk* corners + center (catches small biome slivers cutting
    // through the chunk that don't reach the extended corners). Only if all
    // 9 agree do we treat the chunk as uniform and skip the IDW pass.
    // 4-corner alone was wrong: thin biome bands could pass between the
    // extended corners undetected, producing chunks that snapped to one
    // biome's amplitude while their neighbors blended correctly → hard step.
    const int xL = x0, xH = x0 + kChunkSize - 1;
    const int zL = z0, zH = z0 + kChunkSize - 1;
    const int xC = x0 + kChunkSize / 2;
    const int zC = z0 + kChunkSize / 2;
    Biome bRef = getCachedBiome(xL - P, zL - P);
    bool uniformBiome =
        bRef == getCachedBiome(xH + P, zL - P) &&
        bRef == getCachedBiome(xL - P, zH + P) &&
        bRef == getCachedBiome(xH + P, zH + P) &&
        bRef == getCachedBiome(xL,     zL    ) &&
        bRef == getCachedBiome(xH,     zL    ) &&
        bRef == getCachedBiome(xL,     zH    ) &&
        bRef == getCachedBiome(xH,     zH    ) &&
        bRef == getCachedBiome(xC,     zC    );
    Biome cornerA = bRef; // re-used below by the uniform-path amp lookup

    // Lazy biome cache: only filled if we need to do IDW (non-uniform chunk).
    std::vector<Biome>   biomeCache;
    std::vector<uint8_t> interior;
    auto cacheBiome = [&](int dx, int dz) -> Biome {
        return biomeCache[(size_t)dz * E + dx];
    };

    if (!uniformBiome) {
        // Slow path: sample biome for every cell of the padded region (using the
        // shared biome cache so neighboring chunks reuse work) + interior pass.
        biomeCache.resize((size_t)E * E);
        for (int dz = 0; dz < E; ++dz) {
            for (int dx = 0; dx < E; ++dx) {
                biomeCache[(size_t)dz * E + dx] = getCachedBiome(x0 - P + dx, z0 - P + dz);
            }
        }
        interior.assign((size_t)I * I, 0);
        for (int iz = 0; iz < I; ++iz) {
            for (int ix = 0; ix < I; ++ix) {
                int cdx = ix + B, cdz = iz + B;
                Biome me = cacheBiome(cdx, cdz);
                bool isInt = true;
                for (int r = 1; r <= B && isInt; ++r) {
                    if (cacheBiome(cdx + r, cdz) != me ||
                        cacheBiome(cdx - r, cdz) != me ||
                        cacheBiome(cdx, cdz + r) != me ||
                        cacheBiome(cdx, cdz - r) != me) {
                        isInt = false;
                    }
                }
                interior[(size_t)iz * I + ix] = isInt ? 1 : 0;
            }
        }
    }

    // ── Per-column heightmap: surfaceY = Y_TOP_RAMP_START + (noise0..1) * blendedAmp.
    const float uniformAmp = uniformBiome ? biomeTopRampDepth(cornerA) : 0.0f;
    std::vector<int> surfaceYCache((size_t)kChunkSize * kChunkSize);
    for (int lz = 0; lz < kChunkSize; ++lz) {
        for (int lx = 0; lx < kChunkSize; ++lx) {
            size_t outIdx = (size_t)lz * kChunkSize + lx;

            float amp;
            if (uniformBiome) {
                amp = uniformAmp;
            } else {
                int ix = lx + S, iz = lz + S;
                int cdx = lx + P, cdz = lz + P;
                Biome myBiome = cacheBiome(cdx, cdz);

                if (interior[(size_t)iz * I + ix]) {
                    amp = biomeTopRampDepth(myBiome);
                } else {
                    // IDW blend amplitude from up to 3 nearest distinct-biome interior columns.
                    struct Sample { Biome b; float distSq; };
                    Sample samples[3];
                    int nSamples = 0;
                    for (int r = 0; r <= S && nSamples < 3; ++r) {
                        for (int dz = -r; dz <= r && nSamples < 3; ++dz) {
                            for (int dx = -r; dx <= r && nSamples < 3; ++dx) {
                                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                                int six = ix + dx, siz = iz + dz;
                                if (six < 0 || six >= I || siz < 0 || siz >= I) continue;
                                if (!interior[(size_t)siz * I + six]) continue;
                                Biome b = cacheBiome(six + B, siz + B);
                                bool dup = false;
                                for (int k = 0; k < nSamples; ++k)
                                    if (samples[k].b == b) { dup = true; break; }
                                if (dup) continue;
                                samples[nSamples++] = { b, (float)(dx * dx + dz * dz) };
                            }
                        }
                    }
                    if (nSamples == 0) {
                        amp = biomeTopRampDepth(myBiome);
                    } else {
                        float num = 0.0f, den = 0.0f;
                        for (int k = 0; k < nSamples; ++k) {
                            float w = 1.0f / std::max(samples[k].distSq, 1.0f);
                            num += biomeTopRampDepth(samples[k].b) * w;
                            den += w;
                        }
                        amp = num / den;
                    }
                }
            }

            int wx = x0 + lx, wz = z0 + lz;
            float n = PerlinNoise::sample2D((float)wx * HEIGHTMAP_FREQ_MULT,
                                            (float)wz * HEIGHTMAP_FREQ_MULT); // -1..1
            float t = n * 0.5f + 0.5f;                                        //  0..1
            surfaceYCache[outIdx] = Y_TOP_RAMP_START + (int)(t * amp);
        }
    }

    // ── Two-region fill: 3D-noise terrain below Y_TOP_RAMP_START, heightmap above.
    for (int z = z0; z < z1; ++z) {
        int lz = z - z0;
        for (int y = yLo; y < yHi; ++y) {
            int ly = y - y0;
            bool useHeightmap = (y >= Y_TOP_RAMP_START);

            // Compute the y-only noise bias for the 3D-noise region (unused above 150).
            float bias = 0.0f;
            if (!useHeightmap) {
                if (y >= 0) {
                    // [0, Y_TOP_RAMP_START): linear from BOTTOM (dense) → TOP (sparser).
                    float t = (float)y / (float)Y_TOP_RAMP_START;
                    bias = HEIGHT_MOD_BOTTOM + t * (HEIGHT_MOD_TOP - HEIGHT_MOD_BOTTOM);
                } else if (y >= Y_RAMP_START) {
                    // [Y_RAMP_START, 0): capped at BOTTOM bias.
                    bias = HEIGHT_MOD_BOTTOM;
                } else {
                    // [Y_MIN_WORLD, Y_RAMP_START): bottom fade over 10 blocks → bias +1 (forced air void).
                    float depth = (float)(Y_RAMP_START - y);
                    float pct   = depth / bottomRampDepth;
                    bias = HEIGHT_MOD_BOTTOM + pct * (1.0f - HEIGHT_MOD_BOTTOM);
                }
            }

            for (int x = x0; x < x1; ++x) {
                int lx = x - x0;
                if (useHeightmap) {
                    int surfaceY = surfaceYCache[(size_t)lz * kChunkSize + lx];
                    if (y > surfaceY) {
                        // air above the heightmap surface — nothing to place
                    } else {
                        // Heightmap fill, with a noise-based carve that ramps in:
                        // bias goes from HEIGHT_MOD_TOP at y=150 to HEIGHT_MOD_MAX at y=180+.
                        // High noise → skip the block (removal). This pass never *adds* blocks.
                        float surfaceBias;
                        if (y >= Y_HEIGHT_MAX_END) {
                            surfaceBias = HEIGHT_MOD_MAX;
                        } else {
                            float t = (float)(y - Y_TOP_RAMP_START)
                                    / (float)(Y_HEIGHT_MAX_END - Y_TOP_RAMP_START);
                            surfaceBias = HEIGHT_MOD_TOP + t * (HEIGHT_MOD_MAX - HEIGHT_MOD_TOP);
                        }
                        float ns = PerlinNoise::sample3D((float)x, (float)y, (float)z);
                        float threshold = NOISE_THRESHOLD + surfaceBias;
                        if (ns <= threshold) {
                            out->cells[voxelLocalIdx(lx, ly, lz)] = 1;
                        }
                    }
                } else {
                    float ns = PerlinNoise::sample3D((float)x, (float)y, (float)z);
                    float threshold = NOISE_THRESHOLD + bias;
                    if (ns <= threshold) {
                        out->cells[voxelLocalIdx(lx, ly, lz)] = 1;
                    }
                }
            }
        }
    }
    return out;
}

static void stage1WorkerLoop(int workerIdx) {
    static const glm::ivec3 kNbOffset[6] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };
    while (true) {
        glm::ivec3 chunk;
        {
            std::unique_lock<std::mutex> lk(sStage1Mutex);
            sStage1CV.wait(lk, [] { return !sStage1Queue.empty() || sStopFlag.load(); });
            if (sStopFlag.load() && sStage1Queue.empty()) return;

            int tier = workerIdx % PRIMARY_Y_TIERS;
            int targetY = sPlayerCY.load(std::memory_order_relaxed) + tierToYOffset(tier);
            auto matchIt = sStage1Queue.end();
            for (auto it = sStage1Queue.begin(); it != sStage1Queue.end(); ++it) {
                if (it->y == targetY) { matchIt = it; break; }
            }
            if (matchIt != sStage1Queue.end()) {
                chunk = *matchIt;
                sStage1Queue.erase(matchIt);
            } else {
                chunk = sStage1Queue.front();
                sStage1Queue.pop_front();
            }
        }

        auto voxels = computeChunkVoxels(chunk);
        voxels->stage1Done.store(true, std::memory_order_release);

        {
            std::unique_lock<std::shared_mutex> wlk(sWorkerVoxelsMutex);
            sWorkerVoxels[chunk] = std::move(voxels);
        }

        // Push self + each of 6 neighbors as Stage 2 candidates. The Stage 2
        // worker will check each candidate's actual readiness when it pops.
        {
            std::scoped_lock lk(sStage2Mutex);
            sStage2Queue.push_back(chunk);
            for (const auto& off : kNbOffset)
                sStage2Queue.push_back(chunk + off);
        }
        sStage2CV.notify_all();
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Stage 2: greedy meshing on a voxel snapshot.
// ─────────────────────────────────────────────────────────────────────────
struct VoxelSnapshot {
    glm::ivec3 chunkCoord;
    const WorkerVoxelChunk* self = nullptr;
    const WorkerVoxelChunk* nb[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    // index order matches kNbOffset above: +X, -X, +Y, -Y, +Z, -Z
};

static bool snapCellAt(const VoxelSnapshot& snap, const glm::ivec3& worldPos) {
    glm::ivec3 c = chunkCoordOf(worldPos);
    const WorkerVoxelChunk* v = nullptr;
    if      (c == snap.chunkCoord)                                  v = snap.self;
    else if (c == snap.chunkCoord + glm::ivec3( 1, 0, 0))           v = snap.nb[0];
    else if (c == snap.chunkCoord + glm::ivec3(-1, 0, 0))           v = snap.nb[1];
    else if (c == snap.chunkCoord + glm::ivec3( 0, 1, 0))           v = snap.nb[2];
    else if (c == snap.chunkCoord + glm::ivec3( 0,-1, 0))           v = snap.nb[3];
    else if (c == snap.chunkCoord + glm::ivec3( 0, 0, 1))           v = snap.nb[4];
    else if (c == snap.chunkCoord + glm::ivec3( 0, 0,-1))           v = snap.nb[5];
    if (!v) return false; // missing chunk → treat as empty (boundary face emitted)
    int lx = sceneChunkLocal(worldPos.x);
    int ly = sceneChunkLocal(worldPos.y);
    int lz = sceneChunkLocal(worldPos.z);
    return v->cells[voxelLocalIdx(lx, ly, lz)] != 0;
}

static MeshUpload greedyMeshFromSnapshot(const VoxelSnapshot& snap) {
    MeshUpload up;
    up.chunkCoord = snap.chunkCoord;

    static const glm::ivec3 kFaceOffset[6] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };
    struct PlaneKey { int faceDir, planeIntCoord; };
    struct PlaneKeyHash {
        size_t operator()(const PlaneKey& k) const {
            return (size_t)(k.faceDir * 73856093) ^ (size_t)(k.planeIntCoord * 19349663);
        }
    };
    struct PlaneKeyEq {
        bool operator()(const PlaneKey& a, const PlaneKey& b) const {
            return a.faceDir == b.faceDir && a.planeIntCoord == b.planeIntCoord;
        }
    };
    std::unordered_map<PlaneKey, std::vector<VEGreedy::GMCell>, PlaneKeyHash, PlaneKeyEq> planes;

    glm::ivec3 origin = snap.chunkCoord * kChunkSize;
    for (int lz = 0; lz < kChunkSize; lz++) {
        for (int ly = 0; ly < kChunkSize; ly++) {
            for (int lx = 0; lx < kChunkSize; lx++) {
                if (snap.self->cells[voxelLocalIdx(lx, ly, lz)] == 0) continue;
                glm::ivec3 worldPos = origin + glm::ivec3(lx, ly, lz);
                for (int faceDir = 0; faceDir < 6; faceDir++) {
                    glm::ivec3 nbPos = worldPos + kFaceOffset[faceDir];
                    if (snapCellAt(snap, nbPos)) continue; // covered → cull
                    int planeCoord = VEGreedy::planeIntCoordFor(faceDir, worldPos);
                    const VEGreedy::FaceAxes& axes = VEGreedy::kFaceAxes[faceDir];
                    VEGreedy::GMCell cell;
                    cell.u = worldPos[axes.uAxis];
                    cell.v = worldPos[axes.vAxis];
                    cell.paletteIndex = 0;
                    planes[{faceDir, planeCoord}].push_back(cell);
                }
            }
        }
    }

    auto emit = [&](int faceDir, int planeIntCoord,
                    int uMin, int uMax, int vMin, int vMax,
                    int /*paletteIndex*/) {
        const VEGreedy::FaceAxes& axes = VEGreedy::kFaceAxes[faceDir];
        float planeReal = planeIntCoord * 0.5f;
        float u0 = (float)uMin - 0.5f, u1 = (float)uMax + 0.5f;
        float v0 = (float)vMin - 0.5f, v1 = (float)vMax + 0.5f;
        auto makeCorner = [&](float uu, float vv) {
            glm::vec3 p(0.0f);
            p[axes.normalAxis] = planeReal;
            p[axes.uAxis]      = uu;
            p[axes.vAxis]      = vv;
            return p;
        };
        glm::vec3 c00 = makeCorner(u0, v0);
        glm::vec3 c10 = makeCorner(u1, v0);
        glm::vec3 c11 = makeCorner(u1, v1);
        glm::vec3 c01 = makeCorner(u0, v1);
        const glm::vec3& normal = VEGreedy::kCardinalDirs[faceDir];
        glm::vec3 testCross = glm::cross(c10 - c00, c11 - c00);
        bool ccwGivesOutward = glm::dot(testCross, normal) > 0.0f;
        unsigned int base = (unsigned int)(up.verts.size() / 8);
        auto pushVert = [&](const glm::vec3& p, float uu, float vv) {
            up.verts.push_back(p.x); up.verts.push_back(p.y); up.verts.push_back(p.z);
            up.verts.push_back(uu);  up.verts.push_back(vv);
            up.verts.push_back(normal.x); up.verts.push_back(normal.y); up.verts.push_back(normal.z);
        };
        if (ccwGivesOutward) {
            pushVert(c00, 0.0f, 0.0f);
            pushVert(c10, 1.0f, 0.0f);
            pushVert(c11, 1.0f, 1.0f);
            pushVert(c01, 0.0f, 1.0f);
            up.indices.push_back(base);     up.indices.push_back(base + 1); up.indices.push_back(base + 2);
            up.indices.push_back(base);     up.indices.push_back(base + 2); up.indices.push_back(base + 3);
        } else {
            pushVert(c00, 0.0f, 0.0f);
            pushVert(c11, 1.0f, 1.0f);
            pushVert(c10, 1.0f, 0.0f);
            pushVert(c01, 0.0f, 1.0f);
            up.indices.push_back(base);     up.indices.push_back(base + 1); up.indices.push_back(base + 2);
            up.indices.push_back(base);     up.indices.push_back(base + 3); up.indices.push_back(base + 1);
        }
    };
    for (const auto& [key, cells] : planes)
        VEGreedy::greedyMeshPlane(key.faceDir, key.planeIntCoord, cells, emit);

    return up;
}

static void stage2WorkerLoop() {
    static const glm::ivec3 kNb6[6] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
    };
    while (true) {
        glm::ivec3 chunk;
        {
            std::unique_lock<std::mutex> lk(sStage2Mutex);
            sStage2CV.wait(lk, [] { return !sStage2Queue.empty() || sStopFlag.load(); });
            if (sStopFlag.load() && sStage2Queue.empty()) return;
            chunk = sStage2Queue.front();
            sStage2Queue.pop_front();
        }

        VoxelSnapshot snap;
        snap.chunkCoord = chunk;
        bool ready = false;
        bool allNbReady = true; // becomes false if any neighbor is missing or stage1-pending
        WorkerVoxelChunk* selfChunk = nullptr;
        {
            std::shared_lock<std::shared_mutex> rlk(sWorkerVoxelsMutex);
            auto sit = sWorkerVoxels.find(chunk);
            if (sit != sWorkerVoxels.end() && sit->second->stage1Done.load(std::memory_order_acquire)) {
                // Skip if a prior pass already produced a "final" mesh (all 6
                // neighbors were present) — re-meshing would produce identical
                // output and just orphan more arena bytes.
                if (sit->second->stage2Done.load(std::memory_order_acquire)) continue;
                selfChunk = sit->second.get();
                snap.self = selfChunk;
                ready = true;
                for (int i = 0; i < 6; i++) {
                    auto nit = sWorkerVoxels.find(chunk + kNb6[i]);
                    if (nit != sWorkerVoxels.end() &&
                        nit->second->stage1Done.load(std::memory_order_acquire)) {
                        snap.nb[i] = nit->second.get();
                    } else {
                        snap.nb[i] = nullptr; // missing → treated as empty by snapCellAt
                        allNbReady = false;
                    }
                }
            }
            // Greedy reads voxels through `snap` while the read lock is held.
            // Stage 1 only INSERTS new entries (never modifies existing), so the
            // pointers we captured stay valid for the duration of this block.
            if (ready) {
                MeshUpload up = greedyMeshFromSnapshot(snap);
                {
                    std::scoped_lock ulk(sMeshUploadMutex);
                    sMeshUploadQueue.push(std::move(up));
                }
                // Mark this mesh as final only if every neighbor was present —
                // otherwise we want a future push (after the missing neighbor
                // finishes Stage 1) to re-mesh with full boundary information.
                if (allNbReady && selfChunk) {
                    selfChunk->stage2Done.store(true, std::memory_order_release);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Worker pool lifecycle
// ─────────────────────────────────────────────────────────────────────────
// Logical threads kept off the gen pool for the main thread + future game
// systems (entities, physics, weather, time, AI). Lower = more gen workers,
// less headroom for the game loop.
static const int   RESERVED_FOR_SYSTEMS = 2;

static void startWorkers() {
    sStopFlag.store(false);
    int n = (int)std::thread::hardware_concurrency();
    if (n <= 0) n = 2;
    n = std::max(2, n - RESERVED_FOR_SYSTEMS); // use everything available
    int n2 = std::max(1, n / 4);               // ~25% of workers do greedy
    int n1 = std::max(1, n - n2);
    for (int i = 0; i < n1; i++)
        sStage1Workers.emplace_back(stage1WorkerLoop, i);
    for (int i = 0; i < n2; i++)
        sStage2Workers.emplace_back(stage2WorkerLoop);
}

static void stopWorkers() {
    sStopFlag.store(true);
    sStage1CV.notify_all();
    sStage2CV.notify_all();
    for (auto& t : sStage1Workers) if (t.joinable()) t.join();
    for (auto& t : sStage2Workers) if (t.joinable()) t.join();
    sStage1Workers.clear();
    sStage2Workers.clear();
    {
        std::scoped_lock lk(sStage1Mutex);
        sStage1Queue.clear();
    }
    {
        std::scoped_lock lk(sStage2Mutex);
        sStage2Queue.clear();
    }
    {
        std::scoped_lock lk(sMeshUploadMutex);
        std::queue<MeshUpload> empty;
        std::swap(sMeshUploadQueue, empty);
    }
    {
        std::unique_lock<std::shared_mutex> wlk(sWorkerVoxelsMutex);
        sWorkerVoxels.clear();
    }
    sInFlight.clear();
}

// ─────────────────────────────────────────────────────────────────────────
// Scene callbacks
// ─────────────────────────────────────────────────────────────────────────
static bool sWireframe = false;
static bool sWasComboDown = false;
static bool sSuperSpeed = false;
static bool sWasEDown = false;
static const float BASE_MOVE_SPEED   = 25.0f;
static const float SUPER_SPEED_MULT  = 5.0f;

// Rolling benchmark: every N chunks finished by Stage 3, print wall-clock time
// since the previous batch started. Useful for spotting throughput regressions.
static double sBenchmarkBatchStart = 0.0;
static int    sBenchmarkCounter    = 0;
static const int BENCHMARK_CHUNK_COUNT = 100;

static void onEnter(std::shared_ptr<void>) {
    VE::setCamera(GRID * 1.5f, 230.0f, GRID * 1.5f, 225.0f, -25.0f);
    VE::setMoveSpeed(sSuperSpeed ? BASE_MOVE_SPEED * SUPER_SPEED_MULT : BASE_MOVE_SPEED);
    VE::setGradientBackground(true,
        glm::vec3(0.05f, 0.08f, 0.18f),
        FOG_COLOR);
    VE::setBrightness(1.0f);
    VE::setSpecularStrength(0.05f);
    VE::setFog(true, FOG_COLOR, FOG_START, FOG_END);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    VE::clearDraws(); // empties engine voxels (we won't populate them) and queues a rebuild

    sPendingChunks.clear();
    sPendingSet.clear();
    sBuilt.clear();
    sInFlight.clear();
    sChunkToMergedIdx.clear();
    sFreeMergedSlots.clear();
    sEvictQueue.clear();
    ctx.mergedMeshes.clear();

    sBenchmarkBatchStart = glfwGetTime();
    sBenchmarkCounter    = 0;

    // Pre-size the biome cache so the hash map doesn't rehash partway through
    // streaming. ~50k entries fits a comfortable view radius of unique columns.
    reserveBiomeCache(50000);

    startWorkers();
}

static void onExit() {
    stopWorkers();
    clearBiomeCache();
    ctx.mergedMeshes.clear(); // releases all per-chunk Mesh shared_ptrs → ~Mesh deletes their GL buffers
    glDisable(GL_CULL_FACE);
}

static void onInput(float) {
    bool ctrlDown = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                 || glfwGetKey(ctx.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    bool oneDown  = glfwGetKey(ctx.window, GLFW_KEY_1) == GLFW_PRESS;
    bool wireCombo = ctrlDown && oneDown;
    if (wireCombo && !sWasComboDown) {
        sWireframe = !sWireframe;
        glPolygonMode(GL_FRONT_AND_BACK, sWireframe ? GL_LINE : GL_FILL);
    }
    sWasComboDown = wireCombo;

    bool eDown = glfwGetKey(ctx.window, GLFW_KEY_E) == GLFW_PRESS;
    if (eDown && !sWasEDown) {
        sSuperSpeed = !sSuperSpeed;
        VE::setMoveSpeed(sSuperSpeed ? BASE_MOVE_SPEED * SUPER_SPEED_MULT : BASE_MOVE_SPEED);
    }
    sWasEDown = eDown;
}

static void onUpdate() {
    Camera* cam = getGlobalCamera();
    glm::vec3 camPos = cam->position;
    glm::vec3 fwdRaw = cam->target - cam->position;
    float fwdLen = glm::length(fwdRaw);
    glm::vec3 camFwd = (fwdLen > 0.001f) ? (fwdRaw / fwdLen) : glm::vec3(0.0f, 0.0f, -1.0f);
    const float viewD2 = VIEW_DIST * VIEW_DIST;

    // ── Cull stale pending chunks. Anything queued by past discovery passes
    // that's now past KEEP_DIST will never be dispatched (the dispatcher
    // filters by view distance) but used to sit in sPendingChunks /
    // sPendingSet forever — pure memory leak proportional to total travel.
    {
        const float keepD2 = KEEP_DIST * KEEP_DIST;
        size_t writeIdx = 0;
        for (size_t i = 0; i < sPendingChunks.size(); ++i) {
            const glm::ivec3& c = sPendingChunks[i];
            glm::vec3 d = chunkCenter(c) - camPos;
            d.y *= 2.0f;
            if (glm::dot(d, d) > keepD2) {
                sPendingSet.erase(c);
            } else {
                sPendingChunks[writeIdx++] = c;
            }
        }
        sPendingChunks.resize(writeIdx);
    }

    // ── Discovery (main): enqueue any in-range chunk that's not built / pending / in-flight.
    int camCX = (int)std::floor(camPos.x / (float)kChunkSize);
    int camCZ = (int)std::floor(camPos.z / (float)kChunkSize);
    int radiusXZ = (int)std::ceil(VIEW_DIST / (float)kChunkSize) + 1;
    for (int cy = CHUNK_Y_MIN; cy <= CHUNK_Y_MAX; ++cy) {
        for (int dz = -radiusXZ; dz <= radiusXZ; ++dz) {
            for (int dx = -radiusXZ; dx <= radiusXZ; ++dx) {
                glm::ivec3 c(camCX + dx, cy, camCZ + dz);
                glm::vec3 d = chunkCenter(c) - camPos;
                d.y *= 2.0f;
                if (glm::dot(d, d) > viewD2) continue;
                if (sBuilt.count(c) || sPendingSet.count(c) || sInFlight.count(c)) continue;
                sPendingChunks.push_back(c);
                sPendingSet.insert(c);
            }
        }
    }

    // ── Dispatch (main): round-robin across concentric Y rings. Push selected
    // chunks into Stage 1's queue.
    int playerCY = (int)std::floor(camPos.y / (float)kChunkSize);
    sPlayerCY.store(playerCY, std::memory_order_relaxed);
    const int kMaxInFlight = (int)sStage1Workers.size() * 3;
    const int maxBelow = playerCY - CHUNK_Y_MIN;
    const int maxAbove = CHUNK_Y_MAX - playerCY;
    const int maxTier = 2 * std::max(maxBelow, maxAbove);
    while ((int)sInFlight.size() < kMaxInFlight) {
        bool dispatchedAny = false;
        for (int tier = 0; tier <= maxTier; ++tier) {
            if ((int)sInFlight.size() >= kMaxInFlight) break;
            int targetY = playerCY + tierToYOffset(tier);
            if (targetY < CHUNK_Y_MIN || targetY > CHUNK_Y_MAX) continue;

            auto bestIt = sPendingChunks.end();
            float bestScore = std::numeric_limits<float>::max();
            for (auto it = sPendingChunks.begin(); it != sPendingChunks.end(); ++it) {
                if (it->y != targetY) continue;
                glm::vec3 d = chunkCenter(*it) - camPos;
                glm::vec3 dCull = d;
                dCull.y *= 2.0f;
                float dist2 = glm::dot(dCull, dCull);
                if (dist2 > viewD2) continue;

                // Forward-direction bias: chunks the camera is looking at cost
                // less than chunks beside or behind it. Cosine of the angle
                // between (chunkPos - camera) and camera-forward:
                //   +1 = directly in front, 0 = perpendicular, -1 = behind.
                // Multiplier 1 (front) ↔ 2 (side) ↔ 3 (behind).
                float dLen = std::sqrt(glm::dot(d, d));
                float cosA = (dLen > 0.001f) ? (glm::dot(d, camFwd) / dLen) : 1.0f;
                float score = dist2 * (2.0f - cosA);

                if (score < bestScore) { bestScore = score; bestIt = it; }
            }
            if (bestIt == sPendingChunks.end()) continue;

            glm::ivec3 chunk = *bestIt;
            *bestIt = sPendingChunks.back();
            sPendingChunks.pop_back();
            sPendingSet.erase(chunk);
            sInFlight.insert(chunk);
            {
                std::scoped_lock lk(sStage1Mutex);
                sStage1Queue.push_back(chunk);
            }
            sStage1CV.notify_one();
            dispatchedAny = true;
        }
        if (!dispatchedAny) break;
    }

    // ── Drain the voxel-eviction queue: chunks whose 6-neighbor ring has been
    // meshed and whose delay has elapsed get their 4 KB voxel array freed.
    {
        double now = glfwGetTime();
        std::unique_lock<std::shared_mutex> wlk(sWorkerVoxelsMutex);
        while (!sEvictQueue.empty() && sEvictQueue.front().readyAt <= now) {
            sWorkerVoxels.erase(sEvictQueue.front().chunk);
            sEvictQueue.pop_front();
        }
    }

    // ── Distance-based chunk eviction. Anything past KEEP_DIST from the
    // camera gets dropped from every container. We iterate sBuilt (the
    // superset of every chunk that's been processed by Stage 3) — empty
    // chunks (sky/bedrock) live there but NOT in sChunkToMergedIdx, so
    // iterating the map would leak them. World-boundary chunks also can't
    // hit the 6-neighbor eviction path (missing neighbors are never built),
    // so distance is the only thing that can clean them up.
    {
        const float keepD2 = KEEP_DIST * KEEP_DIST;
        std::vector<glm::ivec3> toErase;
        toErase.reserve(256);
        for (const glm::ivec3& c : sBuilt) {
            glm::vec3 d = chunkCenter(c) - camPos;
            d.y *= 2.0f;
            if (glm::dot(d, d) > keepD2) toErase.push_back(c);
        }
        if (!toErase.empty()) {
            std::unique_lock<std::shared_mutex> wlk(sWorkerVoxelsMutex);
            for (const glm::ivec3& c : toErase) {
                auto it = sChunkToMergedIdx.find(c);
                if (it != sChunkToMergedIdx.end()) {
                    // Non-empty chunk: empty its slot and stash the index
                    // for re-use by the next new chunk.
                    ctx.mergedMeshes[it->second] = MergedMeshEntry{};
                    sFreeMergedSlots.push_back(it->second);
                    sChunkToMergedIdx.erase(it);
                }
                sBuilt.erase(c);
                sWorkerVoxels.erase(c);
            }
        }
    }

    // ── Stage 3 (main): drain mesh-upload queue. Each entry is a vertex/index
    // pair from greedy meshing — we just create a Mesh (the GL upload) and
    // splice it into ctx.mergedMeshes.
    double drainStart = glfwGetTime();
    int drained = 0;
    while (drained < MAX_CHUNKS_PER_FRAME) {
        if (drained > 0) {
            double elapsedMs = (glfwGetTime() - drainStart) * 1000.0;
            if (elapsedMs >= BUILD_BUDGET_MS) break;
        }
        MeshUpload up;
        bool got = false;
        {
            std::scoped_lock lk(sMeshUploadMutex);
            if (!sMeshUploadQueue.empty()) {
                up = std::move(sMeshUploadQueue.front());
                sMeshUploadQueue.pop();
                got = true;
            }
        }
        if (!got) break;

        sInFlight.erase(up.chunkCoord);
        sBuilt.insert(up.chunkCoord);

        // Schedule voxel eviction for any chunk that just had its 6-neighbor
        // ring completed. The chunk itself plus each of its 6 neighbors
        // could now be evictable — check all 7.
        static const glm::ivec3 kNbEvict[6] = {
            { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0},
            { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1},
        };
        auto allNeighborsBuilt = [&](const glm::ivec3& c) {
            for (const auto& o : kNbEvict) if (!sBuilt.count(c + o)) return false;
            return true;
        };
        double evictReadyAt = glfwGetTime() + EVICT_DELAY_SECONDS;
        if (allNeighborsBuilt(up.chunkCoord))
            sEvictQueue.push_back({up.chunkCoord, evictReadyAt});
        for (const auto& o : kNbEvict) {
            glm::ivec3 nb = up.chunkCoord + o;
            if (sBuilt.count(nb) && allNeighborsBuilt(nb))
                sEvictQueue.push_back({nb, evictReadyAt});
        }

        if (++sBenchmarkCounter >= BENCHMARK_CHUNK_COUNT) {
            double now = glfwGetTime();
            double elapsed = now - sBenchmarkBatchStart;
            // Sample every potentially-growing container so we can see if any
            // are leaking with movement. If a number keeps climbing across
            // many batches even while you stand still or fly in circles,
            // that container has a leak.
            size_t qUpload, qStage1, qStage2, voxelMapSize;
            {
                std::scoped_lock lk(sMeshUploadMutex);
                qUpload = sMeshUploadQueue.size();
            }
            {
                std::scoped_lock lk(sStage1Mutex);
                qStage1 = sStage1Queue.size();
            }
            {
                std::scoped_lock lk(sStage2Mutex);
                qStage2 = sStage2Queue.size();
            }
            {
                std::shared_lock<std::shared_mutex> rlk(sWorkerVoxelsMutex);
                voxelMapSize = sWorkerVoxels.size();
            }
            std::cout << "[bench] " << BENCHMARK_CHUNK_COUNT << " chunks in "
                      << (elapsed * 1000.0) << "ms"
                      << " | merged=" << ctx.mergedMeshes.size()
                      << " freeSlots=" << sFreeMergedSlots.size()
                      << " live=" << sChunkToMergedIdx.size()
                      << " built=" << sBuilt.size()
                      << " pending=" << sPendingChunks.size()
                      << " pendSet=" << sPendingSet.size()
                      << " inflight=" << sInFlight.size()
                      << " voxels=" << voxelMapSize
                      << " evictQ=" << sEvictQueue.size()
                      << " biome=" << biomeCacheSize()
                      << " s1Q=" << qStage1
                      << " s2Q=" << qStage2
                      << " upQ=" << qUpload
                      << std::endl;
            sBenchmarkBatchStart = now;
            sBenchmarkCounter    = 0;
        }

        if (up.verts.empty()) {
            // Empty greedy result (chunk is fully empty or fully occluded).
            // Erase any prior entry but otherwise emit nothing.
            auto it = sChunkToMergedIdx.find(up.chunkCoord);
            if (it != sChunkToMergedIdx.end()) {
                size_t idx = it->second;
                ctx.mergedMeshes[idx] = MergedMeshEntry{};
            }
            drained++;
            continue;
        }

        // Standalone Mesh per chunk: own VAO/VBO/EBO created here, deleted by
        // ~Mesh when the shared_ptr drops on eviction. Memory is then trivially
        // bounded by the live-chunk count, no manual reclamation needed.
        auto mesh = std::make_shared<Mesh>(up.verts.data(), (int)(up.verts.size() / 8),
                                           up.indices.data(), (int)up.indices.size(), true);
        MergedMeshEntry e;
        e.mesh = mesh;
        e.boundsCenter = chunkCenter(up.chunkCoord);
        e.boundsRadius = (float)kChunkSize * 0.8660254f;

        auto it = sChunkToMergedIdx.find(up.chunkCoord);
        if (it != sChunkToMergedIdx.end()) {
            ctx.mergedMeshes[it->second] = std::move(e);
        } else if (!sFreeMergedSlots.empty()) {
            // Reuse a slot freed by a previous eviction so the vector stops
            // growing indefinitely as the player explores.
            size_t slot = sFreeMergedSlots.back();
            sFreeMergedSlots.pop_back();
            sChunkToMergedIdx[up.chunkCoord] = slot;
            ctx.mergedMeshes[slot] = std::move(e);
        } else {
            sChunkToMergedIdx[up.chunkCoord] = ctx.mergedMeshes.size();
            ctx.mergedMeshes.push_back(std::move(e));
        }
        drained++;
    }

    // We're driving ctx.mergedMeshes ourselves — make sure the engine doesn't
    // wipe it via auto-rebuild. (VE::clearDraws in onEnter set this true; we
    // intentionally suppress every subsequent rebuild.)
    ctx.needsRebuild = false;
}
static void onRender() {}

void registerVisualizerScene() {
    VE::registerScene("visualizer", onEnter, onExit, onInput, onUpdate, onRender);
}
