#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../modelEditor/src/prefabs/3D_modeler/prefab_cube.h"
#include "perlinNoiseManagement.h"
#include "scene.h"

int main() {
    VE::initWindow(1280, 800, "Perlin Noise Visualizer", true);
    VE::setCollidersEnabled(false);
    VE::reserveBlockCapacity(100 * 100 * 100);

    registerMeshWithStates("cube", cubeVertices, 24, cubeIndices, 12,
                           cubeFaceStates, nullptr, false);

    VE::setMode(VE::CHUNK);

    PerlinNoise::setSeed(42);
    PerlinNoise::setScale(0.12f);

    registerVisualizerScene();
    VE::setScene("visualizer");
    VE::run();
    return 0;
}
