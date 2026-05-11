#pragma once

#include <string>
#include <functional>
#include <memory>
#include <glm/glm.hpp>

namespace VE {

enum MeshMode {
    SINGLE,       // one mesh rendered as-is (models, props)
    CHUNK,        // voxel grid backed by per-block DrawInstance, supports
                  // mixed meshes / rotations / paint-color buckets
    CHUNK_VOXEL,  // memory-optimized voxel grid: per-chunk flat byte array,
                  // single-mesh, no rotation. ~150x less RAM per dense chunk.
};

struct MeshDef {
    float* vertices;
    int vertexCount;
    unsigned int* indices;
    int indexCount;
    const char* texturePath;
};

bool initWindow(int width, int height, const char* title, bool maximized = false);
void setCamera(float x, float y, float z,
               float yaw, float pitch);
void setMoveSpeed(float speed);
void setCollidersEnabled(bool enabled);
void reserveBlockCapacity(int approximateBlocks);
void loadMesh(const char* name, const MeshDef& def);
void loadMesh(const char* name, const char* meshFilePath);
void loadMeshDir(const char* dirPath);
void setMode(MeshMode mode);
void draw(const char* meshName, float x, float y, float z,
          float rx = 0.0f, float ry = 0.0f, float rz = 0.0f);
void undraw(float x, float y, float z);
void clearDraws();
void rebuild();
bool hasBlockAt(int x, int y, int z);
void registerScene(const std::string& name, std::function<void(std::shared_ptr<void>)> onEnter,
                   std::function<void()> onExit,
                   std::function<void(float dt)> onInput,
                   std::function<void()> onUpdate,
                   std::function<void()> onRender);
void setScene(const std::string& name, std::shared_ptr<void> data = nullptr);
void setBrightness(float brightness);
void setSpecularStrength(float strength);
void setFog(bool enable, glm::vec3 color, float start, float end);
void setFarPlane(float farPlane);
// Engine-managed hotkey that cycles to the next registered scene (in registration order).
// Triggers on rising edge so a held key only fires once. Pass key=0 to disable.
// modKey=0 means no modifier required. If modKey is GLFW_KEY_LEFT_CONTROL or
// GLFW_KEY_RIGHT_CONTROL (similarly SHIFT/ALT), either left/right variant satisfies it.
void setSceneCycleHotkey(int key, int modKey = 0);
void undrawChunkAt(int chunkX, int chunkY, int chunkZ);
void setGradientBackground(bool enable, glm::vec3 top = glm::vec3(0.0f), glm::vec3 bottom = glm::vec3(0.7f));
void run();

} // namespace VE
