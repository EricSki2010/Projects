#pragma once

#include "../render.h"
#include "../../VisualEngine.h"
#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>

struct RegisteredMesh {
    std::vector<float> vertices;     // pos3+uv2 (5 floats) or pos3+uv2+normal3 (8 floats)
    int vertexCount;
    int floatsPerVertex = 5;         // 5 = no normals, 8 = has normals
    std::vector<unsigned int> indices;
    int indexCount;
    std::shared_ptr<Texture> texture;
    bool rectangular;
    bool isPrefab = false;           // true = built-in prefab, not editable in vectorMesh
    // Per-triangle face direction: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z, -1=none
    std::vector<int> triFaceDir;
    // Per-face cull state: 0=none/open, 1=partial, 2=solid
    int faceState[6] = {0, 0, 0, 0, 0, 0};
};

struct DrawInstance {
    glm::vec3 position;
    glm::vec3 rotation;  // degrees (rx, ry, rz)
    std::string meshName;
};

struct MergedMeshEntry {
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Texture> texture;
    // Optional bounding sphere for distance culling (e.g. fog cutoff). A
    // negative radius means "no bounds, always draw" — used by SINGLE mode.
    glm::vec3 boundsCenter = glm::vec3(0.0f);
    float boundsRadius = -1.0f;
};

// CHUNK mode parameters. Chunks partition world space into kChunkSize^3 cells.
constexpr int kChunkSize = 16;

inline glm::ivec3 chunkCoordOf(const glm::ivec3& p) {
    auto fd = [](int a, int b) {
        return (a >= 0) ? (a / b) : -((-a + b - 1) / b);
    };
    return { fd(p.x, kChunkSize), fd(p.y, kChunkSize), fd(p.z, kChunkSize) };
}

void registerMesh(const char* name, const VE::MeshDef& def);
void registerMeshFromFile(const char* name, const char* meshFilePath);
void addDrawInstance(const char* meshName, float x, float y, float z,
                     float rx = 0.0f, float ry = 0.0f, float rz = 0.0f);
void removeDrawInstance(float x, float y, float z);
void clearDrawInstances();
// View-distance / streaming support: read or wipe a chunk's instances in bulk.
// getChunkInstances returns nullptr if no instances exist for that chunk.
// clearChunkInstances erases the chunk's bucket and updates the cull cache;
// it does not touch colliders (callers handle those separately, like clearDraws).
const std::vector<DrawInstance>* getChunkInstances(const glm::ivec3& chunkCoord);
void clearChunkInstances(const glm::ivec3& chunkCoord);

// ── CHUNK_VOXEL mode ─────────────────────────────────────────────────
//
// Memory-optimized voxel grid: each chunk is a flat byte array of cell
// presence (0 = empty, non-zero = filled). No DrawInstance bucket, no
// face-cull cache. Constraints: single rectangular mesh per scene, no
// rotation, no per-tri paint. ~4 KB per dense chunk vs ~700 KB in CHUNK.
constexpr int kChunkVoxelCount = kChunkSize * kChunkSize * kChunkSize;
struct ChunkVoxels {
    std::array<uint8_t, kChunkVoxelCount> cells{};
    int  filledCount = 0;
    bool dirty       = true;
    MergedMeshEntry cachedEntry;   // greedy-built mesh, valid when !dirty
};

void     setVoxelAt(int x, int y, int z, uint8_t value);
uint8_t  getVoxelAt(int x, int y, int z);
bool     hasVoxelAt(int x, int y, int z);
void     clearVoxelChunk(const glm::ivec3& chunkCoord);
void     clearAllVoxels();
std::vector<MergedMeshEntry> buildVoxelChunkMeshes();
std::vector<MergedMeshEntry> buildMergedMeshes();  // CHUNK mode: face culling + merge
std::vector<MergedMeshEntry> buildSingleMeshes();  // SINGLE mode: full meshes, no culling
void clearMeshData();
void reserveCullCacheCapacity(int approximateBlocks);
const RegisteredMesh* getRegisteredMesh(const char* name);
void setPaintPalette(const glm::vec3* palette, int count);
const glm::vec3* getPaintPalette();
int getPaintPaletteCount();
// Register mesh with interleaved indices: v0,v1,v2,faceDir, ...
// faceDir: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z, 0xFFFFFFFF=none
// faceStates: per-face cull state [6], 0=open, 1=partial, 2=solid (nullptr = all 0)
void registerMeshWithStates(const char* name, float* vertices, int vertexCount,
                            unsigned int* interleavedIndices, int triCount,
                            const int* faceStates = nullptr,
                            const char* texturePath = nullptr, bool isPrefab = false);

// ── Face culling helpers (shared by buildSingleMeshes + exporter) ──
//
// FaceKey identifies one face of one block by (block position, world-space
// face direction in cardinal index 0..5: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z).
struct FaceKey {
    glm::ivec3 pos;
    int face;
    bool operator==(const FaceKey& o) const { return pos == o.pos && face == o.face; }
};
struct FaceKeyHash {
    size_t operator()(const FaceKey& k) const {
        return std::hash<int>()(k.pos.x) ^ (std::hash<int>()(k.pos.y) << 10)
             ^ (std::hash<int>()(k.pos.z) << 20) ^ (std::hash<int>()(k.face) << 28);
    }
};
using FaceCullSet = std::unordered_set<FaceKey, FaceKeyHash>;

// Build a 90-degree-aligned rotation map: rotMap[localFace] = worldFace.
// Returns false if `rotation` is not a multiple of 90 degrees on each axis.
bool buildFaceRotMap(const glm::vec3& rotation, int rotMap[6]);

// Walks all chunk-bucketed instances + registered face states, computes which
// (pos, worldFace) pairs are hidden by adjacent neighbors. Same logic that
// buildSingleMeshes uses internally; exposed so the exporter can reuse it.
const FaceCullSet& computeFaceCullSet();
