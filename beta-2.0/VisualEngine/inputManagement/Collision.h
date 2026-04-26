#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>

struct Triangle {
    glm::vec3 v0, v1, v2;
};

struct AABB {
    glm::vec3 min;
    glm::vec3 max;
};

struct BlockCollider {
    glm::vec3 position;
    glm::vec3 rotation;               // degrees (rx, ry, rz)
    AABB bounds;
    std::vector<Triangle> triangles;  // empty for rectangular meshes (uses AABB only)
    std::string meshName;
    bool isRectangular;               // true = AABB-only, false = triangle-level
    std::vector<int16_t> triColors;   // global palette index per triangle (pack*16+slot, -1 = unpainted)
};

struct CollisionHit {
    bool hit;
    glm::vec3 point;
    glm::vec3 normal;
    float distance;
    const BlockCollider* collider;
    int triangleIndex;  // index into collider->triangles, or face index for AABB
};

// Check if mesh vertices form a rectangular shape (all verts on AABB faces)
bool isMeshRectangular(const float* vertices, int vertexCount);

void addCollider(const char* meshName, const float* vertices, int vertexCount,
                 const unsigned int* indices, int indexCount,
                 bool rectangular, float x, float y, float z, int floatsPerVertex = 5,
                 float rx = 0.0f, float ry = 0.0f, float rz = 0.0f);
void removeCollider(float x, float y, float z);
void clearColliders();
bool hasColliderAt(int x, int y, int z);
const BlockCollider* getColliderAt(int x, int y, int z);
const std::vector<BlockCollider>& getAllColliders();

CollisionHit raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDist = 50.0f);
void setForceRectangularRaycast(bool force);
bool isForceRectangularRaycast();
