#include "ChunkMesh.h"
#include "../../inputManagement/Collision.h"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_set>
#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>

static glm::vec3 rotatePoint(const glm::vec3& v, const glm::vec3& rotDeg) {
    if (rotDeg.x == 0.0f && rotDeg.y == 0.0f && rotDeg.z == 0.0f)
        return v;
    glm::mat4 m(1.0f);
    m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
    return glm::vec3(m * glm::vec4(v, 1.0f));
}

// ── Mesh registry ───────────────────────────────────────────────────

struct IVec3Hash {
    size_t operator()(const glm::ivec3& v) const {
        return std::hash<int>()(v.x) ^ (std::hash<int>()(v.y) << 10) ^ (std::hash<int>()(v.z) << 20);
    }
};

static std::unordered_map<std::string, RegisteredMesh> gMeshes;
// CHUNK mode partitions instances by chunk coord; SINGLE mode iterates all values.
static std::unordered_map<glm::ivec3, std::vector<DrawInstance>, IVec3Hash> gChunkInstances;

// Per-chunk built mesh cache. Each chunk holds two mesh sets: `inner` (faces
// whose cull state depends only on in-chunk neighbors) and `outer` (faces on
// the chunk boundary planes whose cull state depends on adjacent chunks).
// Dirty flags are tracked separately so a neighbor edit only forces an outer
// rebuild — inner stays cached.
struct ChunkBuild {
    std::vector<MergedMeshEntry> innerMeshes;
    std::vector<MergedMeshEntry> outerMeshes;
    bool innerDirty = true;
    bool outerDirty = true;
};
static std::unordered_map<glm::ivec3, ChunkBuild, IVec3Hash> gChunks;

// Local coord within chunk in [0, kChunkSize), handling negative world coords.
static int chunkLocal(int v) {
    int m = v % kChunkSize;
    return m < 0 ? m + kChunkSize : m;
}

// Mark the inner mesh of chunk(p) dirty, and mark outer dirty for every chunk
// whose outer faces depend on p:
//   • this chunk's own outer iff p sits on this chunk's boundary
//   • the adjacent chunk's outer for each axis where p sits on that boundary
static void markChunksDirtyForBlockEdit(const glm::ivec3& p) {
    glm::ivec3 self = chunkCoordOf(p);
    gChunks[self].innerDirty = true;

    int lx = chunkLocal(p.x);
    int ly = chunkLocal(p.y);
    int lz = chunkLocal(p.z);

    auto bumpOuter = [](const glm::ivec3& c) { gChunks[c].outerDirty = true; };

    if (lx == 0)               { bumpOuter(self); bumpOuter(self + glm::ivec3(-1, 0, 0)); }
    if (lx == kChunkSize - 1)  { bumpOuter(self); bumpOuter(self + glm::ivec3( 1, 0, 0)); }
    if (ly == 0)               { bumpOuter(self); bumpOuter(self + glm::ivec3( 0,-1, 0)); }
    if (ly == kChunkSize - 1)  { bumpOuter(self); bumpOuter(self + glm::ivec3( 0, 1, 0)); }
    if (lz == 0)               { bumpOuter(self); bumpOuter(self + glm::ivec3( 0, 0,-1)); }
    if (lz == kChunkSize - 1)  { bumpOuter(self); bumpOuter(self + glm::ivec3( 0, 0, 1)); }
}
// Variable-size paint palette. Size is always a multiple of 16 (one pack = 16
// colors). Triangle paint indices are global: pack*16 + slotInPack.
static std::vector<glm::vec3> gPaintPalette;
static std::shared_ptr<Texture> gPaletteTexture;

void setPaintPalette(const glm::vec3* palette, int count) {
    if (count <= 0) {
        gPaintPalette.clear();
        gPaletteTexture.reset();
        return;
    }
    gPaintPalette.assign(palette, palette + count);

    std::vector<unsigned char> pixels((size_t)count * 3);
    for (int i = 0; i < count; i++) {
        pixels[i * 3 + 0] = (unsigned char)(std::clamp(palette[i].r, 0.0f, 1.0f) * 255.0f);
        pixels[i * 3 + 1] = (unsigned char)(std::clamp(palette[i].g, 0.0f, 1.0f) * 255.0f);
        pixels[i * 3 + 2] = (unsigned char)(std::clamp(palette[i].b, 0.0f, 1.0f) * 255.0f);
    }
    gPaletteTexture = std::make_shared<Texture>(pixels.data(), count, 1, 3);
}

const glm::vec3* getPaintPalette() {
    return gPaintPalette.empty() ? nullptr : gPaintPalette.data();
}

int getPaintPaletteCount() {
    return (int)gPaintPalette.size();
}

void registerMesh(const char* name, const VE::MeshDef& def) {
    RegisteredMesh reg;
    reg.vertices.assign(def.vertices, def.vertices + def.vertexCount * 5);
    reg.vertexCount = def.vertexCount;
    reg.indices.assign(def.indices, def.indices + def.indexCount);
    reg.indexCount = def.indexCount;
    reg.texture = def.texturePath ? std::make_shared<Texture>(def.texturePath) : nullptr;
    reg.rectangular = isMeshRectangular(def.vertices, def.vertexCount) && def.indexCount == 36;
    // Rectangular meshes default to all faces state 2 (solid)
    if (reg.rectangular)
        for (int i = 0; i < 6; i++) reg.faceState[i] = 2;
    gMeshes[name] = std::move(reg);
}

void registerMeshFromFile(const char* name, const char* meshFilePath) {
    std::ifstream file(meshFilePath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to load mesh: " << meshFilePath << std::endl;
        return;
    }

    // Magic values:
    //   "VN" — has per-vertex normals (8 floats/vert)
    //   "VC" — VN + appended culling data (triFaceDir[triCount] + faceState[6])
    //   (none) — legacy format, 5 floats/vert, no normals
    char magic[2] = {0, 0};
    file.read(magic, 2);
    bool hasNormals    = (magic[0] == 'V' && (magic[1] == 'N' || magic[1] == 'C'));
    bool hasCullingData = (magic[0] == 'V' && magic[1] == 'C');
    if (!hasNormals)
        file.seekg(0); // rewind if no magic

    uint32_t vertexCount = 0, indexCount = 0, texturePathLen = 0;
    file.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
    file.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
    file.read(reinterpret_cast<char*>(&texturePathLen), sizeof(texturePathLen));
    if (!file) return;

    if (vertexCount > 1000000 || indexCount > 3000000 || texturePathLen > 4096) {
        std::cerr << "Mesh file has invalid header: " << meshFilePath << std::endl;
        return;
    }

    RegisteredMesh reg;
    reg.vertexCount = vertexCount;
    reg.indexCount = indexCount;
    reg.floatsPerVertex = hasNormals ? 8 : 5;
    reg.vertices.resize(vertexCount * reg.floatsPerVertex);
    reg.indices.resize(indexCount);

    file.read(reinterpret_cast<char*>(reg.vertices.data()), vertexCount * reg.floatsPerVertex * sizeof(float));
    file.read(reinterpret_cast<char*>(reg.indices.data()), indexCount * sizeof(uint32_t));

    std::string texPath(texturePathLen, '\0');
    file.read(texPath.data(), texturePathLen);
    if (!file && texturePathLen > 0) return;

    if (!texPath.empty() && texPath.back() == '\0') texPath.pop_back();

    reg.texture = !texPath.empty() ? std::make_shared<Texture>(texPath.c_str()) : nullptr;
    reg.rectangular = !hasNormals && isMeshRectangular(reg.vertices.data(), reg.vertexCount) && reg.indexCount == 36;
    if (reg.rectangular)
        for (int i = 0; i < 6; i++) reg.faceState[i] = 2;

    // VC format: per-triangle face direction + 6 face states for culling.
    if (hasCullingData) {
        int triCount = (int)(indexCount / 3);
        std::vector<int32_t> faceDirData(triCount);
        file.read(reinterpret_cast<char*>(faceDirData.data()), triCount * sizeof(int32_t));
        reg.triFaceDir.assign(faceDirData.begin(), faceDirData.end());

        int32_t fs[6] = {0, 0, 0, 0, 0, 0};
        file.read(reinterpret_cast<char*>(fs), 6 * sizeof(int32_t));
        for (int i = 0; i < 6; i++) reg.faceState[i] = fs[i];
    }

    gMeshes[name] = std::move(reg);
}

// ── Incremental cull-set cache ──────────────────────────────────────
//
// gFaceMap and gCullSet are kept in sync with gChunkInstances via the cache
// helpers below. This lets buildMergedMeshes() skip the O(N) full-world cull
// recompute every frame: addDrawInstance only does O(1) work per block.

static const glm::vec3 kCardinalDirs[6] = {
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
};
static const int kOppositeFace[6] = {1, 0, 3, 2, 5, 4};

static std::unordered_map<FaceKey, int, FaceKeyHash> gFaceMap; // (pos, worldFace) -> faceState
static FaceCullSet gCullSet;                                   // hidden faces

static void cacheBlockAdd(const std::string& meshName, const glm::ivec3& pos,
                          const glm::vec3& rotation) {
    auto mit = gMeshes.find(meshName);
    if (mit == gMeshes.end()) return;
    const RegisteredMesh& reg = mit->second;
    int rotMap[6];
    if (!buildFaceRotMap(rotation, rotMap)) return;
    for (int f = 0; f < 6; f++) {
        int state = reg.faceState[f];
        if (state == 0) continue;
        int worldFace = rotMap[f];
        gFaceMap[{pos, worldFace}] = state;

        glm::ivec3 nPos = pos + glm::ivec3(glm::round(kCardinalDirs[worldFace]));
        int oppFace = kOppositeFace[worldFace];
        auto nit = gFaceMap.find({nPos, oppFace});
        if (nit == gFaceMap.end()) continue;
        int nState = nit->second;
        // Same rules as computeFaceCullSet: state 2 hides any state≥1 facing it.
        if ((state == 2 && nState == 2) || (state == 1 && nState == 2))
            gCullSet.insert({pos, worldFace});
        if ((nState == 2 && state == 2) || (nState == 1 && state == 2))
            gCullSet.insert({nPos, oppFace});
    }
}

static void cacheBlockRemove(const std::string& meshName, const glm::ivec3& pos,
                             const glm::vec3& rotation) {
    auto mit = gMeshes.find(meshName);
    if (mit == gMeshes.end()) return;
    const RegisteredMesh& reg = mit->second;
    int rotMap[6];
    if (!buildFaceRotMap(rotation, rotMap)) return;
    for (int f = 0; f < 6; f++) {
        int state = reg.faceState[f];
        if (state == 0) continue;
        int worldFace = rotMap[f];
        gFaceMap.erase({pos, worldFace});
        gCullSet.erase({pos, worldFace});
        // Neighbor's opposite face is no longer occluded by us.
        glm::ivec3 nPos = pos + glm::ivec3(glm::round(kCardinalDirs[worldFace]));
        int oppFace = kOppositeFace[worldFace];
        gCullSet.erase({nPos, oppFace});
    }
}

void addDrawInstance(const char* meshName, float x, float y, float z,
                     float rx, float ry, float rz) {
    glm::ivec3 p((int)roundf(x), (int)roundf(y), (int)roundf(z));
    glm::ivec3 c = chunkCoordOf(p);
    glm::vec3 rot(rx, ry, rz);
    gChunkInstances[c].push_back({glm::vec3(x, y, z), rot, meshName});
    cacheBlockAdd(meshName, p, rot);
    markChunksDirtyForBlockEdit(p);
}

void removeDrawInstance(float x, float y, float z) {
    glm::ivec3 target((int)roundf(x), (int)roundf(y), (int)roundf(z));
    glm::ivec3 c = chunkCoordOf(target);
    auto it = gChunkInstances.find(c);
    if (it == gChunkInstances.end()) return;
    auto& bucket = it->second;
    for (const auto& d : bucket) {
        glm::ivec3 p((int)roundf(d.position.x), (int)roundf(d.position.y), (int)roundf(d.position.z));
        if (p == target) cacheBlockRemove(d.meshName, target, d.rotation);
    }
    bucket.erase(
        std::remove_if(bucket.begin(), bucket.end(), [&](const DrawInstance& d) {
            return glm::ivec3((int)roundf(d.position.x), (int)roundf(d.position.y), (int)roundf(d.position.z)) == target;
        }),
        bucket.end()
    );
    if (bucket.empty()) gChunkInstances.erase(it);
    markChunksDirtyForBlockEdit(target);
}

void clearDrawInstances() {
    gChunkInstances.clear();
    gChunks.clear();
    gFaceMap.clear();
    gCullSet.clear();
}

void clearMeshData() {
    gMeshes.clear();
    gChunkInstances.clear();
    gChunks.clear();
    gFaceMap.clear();
    gCullSet.clear();
}

void reserveCullCacheCapacity(int approximateBlocks) {
    if (approximateBlocks <= 0) return;
    // Each block contributes up to 6 face entries to gFaceMap and up to 6 to
    // gCullSet (interior blocks have all 6 faces culled). Reserve generously
    // so neither container rehashes during a fill.
    size_t cap = (size_t)approximateBlocks * 6;
    gFaceMap.reserve(cap);
    gCullSet.reserve(cap);
}

const RegisteredMesh* getRegisteredMesh(const char* name) {
    auto it = gMeshes.find(name);
    return (it != gMeshes.end()) ? &it->second : nullptr;
}

static const int neighborDir[6][3] = {
    { 0, 0, 1}, { 0, 0,-1}, {-1, 0, 0},
    { 1, 0, 0}, { 0, 1, 0}, { 0,-1, 0},
};

void registerMeshWithStates(const char* name, float* vertices, int vertexCount,
                            unsigned int* interleavedIndices, int triCount,
                            const int* faceStates,
                            const char* texturePath, bool isPrefab) {
    RegisteredMesh reg;
    reg.vertices.assign(vertices, vertices + vertexCount * 5);
    reg.vertexCount = vertexCount;
    reg.texture = texturePath ? std::make_shared<Texture>(texturePath) : nullptr;
    reg.rectangular = (vertexCount == 24);
    reg.isPrefab = isPrefab;

    // Parse interleaved: v0,v1,v2,faceDir, ...
    reg.indices.reserve(triCount * 3);
    reg.triFaceDir.reserve(triCount);
    for (int t = 0; t < triCount; t++) {
        int base = t * 4;
        reg.indices.push_back(interleavedIndices[base + 0]);
        reg.indices.push_back(interleavedIndices[base + 1]);
        reg.indices.push_back(interleavedIndices[base + 2]);
        reg.triFaceDir.push_back((int)interleavedIndices[base + 3]);
    }
    reg.indexCount = triCount * 3;

    // Copy face states
    if (faceStates) {
        for (int i = 0; i < 6; i++) reg.faceState[i] = faceStates[i];
    }

    gMeshes[name] = std::move(reg);
}

// ── Face culling + mesh merging ─────────────────────────────────────

// Cardinal directions / opposite-face table are defined above near the
// cull-set cache so addDrawInstance can use them.

bool buildFaceRotMap(const glm::vec3& rotation, int rotMap[6]) {
    for (int f = 0; f < 6; f++) {
        glm::vec3 rotated = rotatePoint(kCardinalDirs[f], rotation);
        int best = -1;
        float bestDot = 0.5f;
        for (int c = 0; c < 6; c++) {
            float d = glm::dot(rotated, kCardinalDirs[c]);
            if (d > bestDot) { bestDot = d; best = c; }
        }
        if (best < 0) return false;
        rotMap[f] = best;
    }
    return true;
}

const FaceCullSet& computeFaceCullSet() {
    // Returns the incrementally-maintained cull set. addDrawInstance and
    // removeDrawInstance keep gCullSet in sync, so this is O(1) and copy-free.
    return gCullSet;
}

// Determines whether a triangle's (chunk-local position, worldFd) is an outer
// face of the chunk: a face on one of the chunk's 6 boundary planes pointing
// outward. Used to partition emitted geometry between inner and outer meshes.
static bool isOuterFace(const glm::ivec3& pos, int worldFd, const glm::ivec3& chunkOrigin) {
    if (worldFd < 0 || worldFd > 5) return false;
    int lx = pos.x - chunkOrigin.x;
    int ly = pos.y - chunkOrigin.y;
    int lz = pos.z - chunkOrigin.z;
    switch (worldFd) {
        case 0: return lx == kChunkSize - 1; // +X
        case 1: return lx == 0;              // -X
        case 2: return ly == kChunkSize - 1; // +Y
        case 3: return ly == 0;              // -Y
        case 4: return lz == kChunkSize - 1; // +Z
        case 5: return lz == 0;              // -Z
    }
    return false;
}

enum class EmitFilter { All, InnerOnly, OuterOnly };

// Shared emit core. Builds one or more MergedMeshEntries from `insts` (grouped
// by meshName + paint bucket) using `cullSet` to skip hidden faces. Used by
// both buildSingleMeshes (whole world) and buildMergedMeshes (per-chunk inner
// and outer passes).
static void emitMeshesForInstances(
    const std::vector<const DrawInstance*>& insts,
    const FaceCullSet& cullSet,
    std::vector<MergedMeshEntry>& result,
    EmitFilter filter = EmitFilter::All,
    const glm::ivec3& chunkOrigin = glm::ivec3(0))
{
    struct InstanceData {
        const DrawInstance* inst;
        std::string meshName;
        int rotMap[6];
        bool is90Aligned;
    };
    std::vector<InstanceData> instances;
    instances.reserve(insts.size());
    for (const DrawInstance* d : insts) {
        auto mit = gMeshes.find(d->meshName);
        if (mit == gMeshes.end()) continue;
        InstanceData idata;
        idata.inst = d;
        idata.meshName = d->meshName;
        idata.is90Aligned = buildFaceRotMap(d->rotation, idata.rotMap);
        instances.push_back(std::move(idata));
    }

    std::unordered_map<std::string, std::vector<int>> meshGroups;
    for (int i = 0; i < (int)instances.size(); i++)
        meshGroups[instances[i].meshName].push_back(i);

    for (const auto& [name, instIndices] : meshGroups) {
        auto mit = gMeshes.find(name);
        if (mit == gMeshes.end()) continue;
        const RegisteredMesh& reg = mit->second;
        int fpv = reg.floatsPerVertex;

        std::vector<float> verts;
        std::vector<unsigned int> indices;
        struct PaintBucket { std::vector<float> verts; std::vector<unsigned int> indices; };
        std::vector<PaintBucket> paintBuckets((size_t)gPaintPalette.size());

        for (int ii : instIndices) {
            const InstanceData& idata = instances[ii];
            const DrawInstance* inst = idata.inst;
            glm::ivec3 pos((int)roundf(inst->position.x), (int)roundf(inst->position.y), (int)roundf(inst->position.z));
            const BlockCollider* col = getColliderAt(pos.x, pos.y, pos.z);

            std::vector<float> instVerts(reg.vertexCount * fpv);
            for (int v = 0; v < reg.vertexCount; v++) {
                int src = v * fpv;
                glm::vec3 local(reg.vertices[src], reg.vertices[src + 1], reg.vertices[src + 2]);
                glm::vec3 world = rotatePoint(local, inst->rotation) + inst->position;
                instVerts[v * fpv + 0] = world.x;
                instVerts[v * fpv + 1] = world.y;
                instVerts[v * fpv + 2] = world.z;
                instVerts[v * fpv + 3] = reg.vertices[src + 3];
                instVerts[v * fpv + 4] = reg.vertices[src + 4];
                if (fpv == 8) {
                    glm::vec3 n(reg.vertices[src + 5], reg.vertices[src + 6], reg.vertices[src + 7]);
                    glm::vec3 rn = rotatePoint(n, inst->rotation);
                    instVerts[v * fpv + 5] = rn.x;
                    instVerts[v * fpv + 6] = rn.y;
                    instVerts[v * fpv + 7] = rn.z;
                }
            }

            auto emitTri = [&](std::vector<float>& tv, std::vector<unsigned int>& ti,
                               unsigned int i0, unsigned int i1, unsigned int i2) {
                unsigned int base = (unsigned int)(tv.size() / fpv);
                for (int vi : {(int)i0, (int)i1, (int)i2})
                    for (int j = 0; j < fpv; j++)
                        tv.push_back(instVerts[vi * fpv + j]);
                ti.push_back(base); ti.push_back(base + 1); ti.push_back(base + 2);
            };

            int triCount = reg.indexCount / 3;
            for (int t = 0; t < triCount; t++) {
                unsigned int i0 = reg.indices[t * 3], i1 = reg.indices[t * 3 + 1], i2 = reg.indices[t * 3 + 2];

                int fd = (t < (int)reg.triFaceDir.size()) ? reg.triFaceDir[t] : -1;
                int worldFd = (idata.is90Aligned && fd >= 0 && fd < 6) ? idata.rotMap[fd] : fd;

                if (worldFd >= 0 && worldFd < 6) {
                    if (cullSet.count({pos, worldFd}))
                        continue;
                }

                if (filter != EmitFilter::All) {
                    bool outer = isOuterFace(pos, worldFd, chunkOrigin);
                    if (filter == EmitFilter::InnerOnly && outer) continue;
                    if (filter == EmitFilter::OuterOnly && !outer) continue;
                }

                int16_t colorIdx = -1;
                int colorLookup = reg.rectangular ? fd : t;
                if (col && colorLookup >= 0 && colorLookup < (int)col->triColors.size())
                    colorIdx = col->triColors[colorLookup];

                if (colorIdx >= 0 && colorIdx < (int)paintBuckets.size())
                    emitTri(paintBuckets[colorIdx].verts, paintBuckets[colorIdx].indices, i0, i1, i2);
                else
                    emitTri(verts, indices, i0, i1, i2);
            }
        }

        if (!verts.empty()) {
            std::shared_ptr<Mesh> mesh;
            if (fpv == 8)
                mesh = std::make_shared<Mesh>(verts.data(), (int)(verts.size() / 8), indices.data(), (int)indices.size(), true);
            else
                mesh = std::make_shared<Mesh>(verts.data(), (int)(verts.size() / 5), indices.data(), (int)indices.size());
            if (reg.texture) mesh->setTexture(reg.texture.get());
            result.push_back({mesh, reg.texture});
        }

        for (int c = 0; c < (int)paintBuckets.size(); c++) {
            auto& pb = paintBuckets[c];
            if (pb.verts.empty()) continue;
            std::shared_ptr<Mesh> mesh;
            if (fpv == 8)
                mesh = std::make_shared<Mesh>(pb.verts.data(), (int)(pb.verts.size() / 8), pb.indices.data(), (int)pb.indices.size(), true);
            else
                mesh = std::make_shared<Mesh>(pb.verts.data(), (int)(pb.verts.size() / 5), pb.indices.data(), (int)pb.indices.size());
            mesh->setColor(gPaintPalette[c]);
            result.push_back({mesh, nullptr});
        }
    }
}

std::vector<MergedMeshEntry> buildSingleMeshes() {
    // SINGLE mode: rebuilds the full world every call. No caching.
    std::vector<const DrawInstance*> allInsts;
    for (const auto& [chunkCoord, bucket] : gChunkInstances)
        for (const auto& d : bucket)
            allInsts.push_back(&d);

    const FaceCullSet& cullSet = computeFaceCullSet();

    std::vector<MergedMeshEntry> result;
    emitMeshesForInstances(allInsts, cullSet, result);
    return result;
}

std::vector<MergedMeshEntry> buildMergedMeshes() {
    // CHUNK mode: each chunk has separate inner / outer mesh sets and dirty
    // flags. Clean halves keep their cached shared_ptr<Mesh> across rebuild()
    // calls. Inner faces depend only on in-chunk neighbors; outer faces depend
    // on adjacent-chunk neighbors. See markChunksDirtyForBlockEdit for the
    // partition rule.
    const FaceCullSet& cullSet = computeFaceCullSet();

    // Track any chunk that has instances but no entry yet (e.g. after a mode
    // switch wiped gChunks). Default ChunkBuild has both halves dirty.
    for (const auto& [chunkCoord, bucket] : gChunkInstances)
        gChunks.try_emplace(chunkCoord);

    // Drop entries for chunks whose instance bucket is gone — otherwise their
    // stale meshes would still be aggregated into the render list.
    for (auto it = gChunks.begin(); it != gChunks.end();) {
        if (gChunkInstances.find(it->first) == gChunkInstances.end())
            it = gChunks.erase(it);
        else
            ++it;
    }

    for (auto& [chunkCoord, build] : gChunks) {
        if (!build.innerDirty && !build.outerDirty) continue;

        const auto bit = gChunkInstances.find(chunkCoord);
        if (bit == gChunkInstances.end()) {
            build.innerMeshes.clear();
            build.outerMeshes.clear();
            build.innerDirty = false;
            build.outerDirty = false;
            continue;
        }

        std::vector<const DrawInstance*> chunkInsts;
        chunkInsts.reserve(bit->second.size());
        for (const auto& d : bit->second) chunkInsts.push_back(&d);

        glm::ivec3 chunkOrigin = chunkCoord * kChunkSize;

        if (build.innerDirty) {
            build.innerMeshes.clear();
            emitMeshesForInstances(chunkInsts, cullSet, build.innerMeshes,
                                   EmitFilter::InnerOnly, chunkOrigin);
            build.innerDirty = false;
        }
        if (build.outerDirty) {
            build.outerMeshes.clear();
            emitMeshesForInstances(chunkInsts, cullSet, build.outerMeshes,
                                   EmitFilter::OuterOnly, chunkOrigin);
            build.outerDirty = false;
        }
    }

    // Aggregate every chunk's cached inner + outer meshes into the flat list
    // the renderer consumes. shared_ptr makes this cheap (refcount bump only).
    std::vector<MergedMeshEntry> result;
    for (const auto& [chunkCoord, build] : gChunks) {
        for (const auto& entry : build.innerMeshes) result.push_back(entry);
        for (const auto& entry : build.outerMeshes) result.push_back(entry);
    }
    return result;
}
