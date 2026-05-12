#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../modelEditor/src/prefabs/3D_modeler/prefab_cube.h"
#include "perlinNoiseManagement.h"
#include "scene.h"
#include "noise2D.h"

#include <GLFW/glfw3.h>

// Single entry-point for switching scenes that also locks in the world seed.
// Use this instead of VE::setScene directly so the noise system is always
// consistent with the active scene.
static void setScene(const char* name, int seed) {
    PerlinNoise::setSeed(seed);
    VE::setScene(name);
}

int main() {
    VE::initWindow(1280, 800, "Perlin Noise Visualizer", true);
    VE::setCollidersEnabled(false);
    VE::reserveBlockCapacity(100 * 100 * 100);

    registerMeshWithStates("cube", cubeVertices, 24, cubeIndices, 12,
                           cubeFaceStates, nullptr, false);

    VE::setMode(VE::CHUNK_VOXEL);
    PerlinNoise::setScale(0.050f);

    registerVisualizerScene();
    registerNoise2DScene();
    VE::setSceneCycleHotkey(GLFW_KEY_2, GLFW_KEY_LEFT_CONTROL); // Ctrl+2 cycles scenes
    setScene("noise2D", 10); // first arg = scene name, second arg = world seed
    VE::run();
    return 0;
}
