#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../modelEditor/src/prefabs/3D_modeler/prefab_cube.h"
#include "perlinNoiseManagement.h"
#include "scene.h"
#include "noise2D.h"

#include <GLFW/glfw3.h>

int main() {
    VE::initWindow(1280, 800, "Perlin Noise Visualizer", true);
    VE::setCollidersEnabled(false);
    VE::reserveBlockCapacity(100 * 100 * 100);

    registerMeshWithStates("cube", cubeVertices, 24, cubeIndices, 12,
                           cubeFaceStates, nullptr, false);

    VE::setMode(VE::CHUNK_VOXEL);

    PerlinNoise::setSeed(42);
    PerlinNoise::setScale(0.050f);

    registerVisualizerScene();
    registerNoise2DScene();
    VE::setSceneCycleHotkey(GLFW_KEY_2, GLFW_KEY_LEFT_CONTROL); // Ctrl+2 cycles scenes
    VE::setScene("noise2D"); // swap to "visualizer" for the 3D Perlin scene
    VE::run();
    return 0;
}
