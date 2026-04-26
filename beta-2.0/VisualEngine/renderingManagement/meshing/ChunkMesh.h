#pragma once

#include "../render.h"
#include "../../VisualEngine.h"
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
    std::unique_ptr<Mesh> mesh;
    std::shared_ptr<Texture> texture;
};

void registerMesh(const char* name, const VE::MeshDef& def);
void registerMeshFromFile(const char* name, const char* meshFilePath);
void addDrawInstance(const char* meshName, float x, float y, float z,
                     float rx = 0.0f, float ry = 0.0f, float rz = 0.0f);
void removeDrawInstance(float x, float y, float z);
void clearDrawInstances();
std::vector<MergedMeshEntry> buildMergedMeshes();  // CHUNK mode: face culling + merge
std::vector<MergedMeshEntry> buildSingleMeshes();  // SINGLE mode: full meshes, no culling
void clearMeshData();
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

// Walks gDrawList + registered face states, computes which (pos, worldFace)
// pairs are hidden by adjacent neighbors. Same logic that buildSingleMeshes
// uses internally; exposed so the exporter can reuse it.
FaceCullSet computeFaceCullSet();
