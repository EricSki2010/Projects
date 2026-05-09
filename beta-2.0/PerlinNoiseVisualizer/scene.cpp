#include "scene.h"
#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/EngineGlobals.h"
#include "../VisualEngine/inputManagement/Camera.h"
#include "../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "perlinNoiseManagement.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

static const int   GRID                 = 100;
static const float NOISE_THRESHOLD      = -0.15f;
static const float NOISE_THRESHOLD_LOOSE = -0.25f;
static const int   SECONDARY_SEED_OFFSET = 100;
static const float SECONDARY_GATE        = -0.05f;
static const float HEIGHT_MOD_BOTTOM     =  0.3f; // applied at y = 0
static const float HEIGHT_MOD_TOP        = -0.1f; // applied at y = GRID - 1
static const int   CHUNKS_PER_FRAME      = 1;

static const int CHUNKS_PER_AXIS = (GRID + kChunkSize - 1) / kChunkSize;
static std::vector<glm::ivec3> sPendingChunks; // chunk coords still to fill

static void onEnter(std::shared_ptr<void>) {
    VE::setCamera(GRID * 1.5f, GRID * 1.2f, GRID * 1.5f, 225.0f, -25.0f);
    VE::setMoveSpeed(25.0f);
    VE::setGradientBackground(true,
        glm::vec3(0.05f, 0.08f, 0.18f),
        glm::vec3(0.55f, 0.70f, 0.95f));
    VE::setBrightness(1.0f);
    VE::setSpecularStrength(0.05f);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    VE::clearDraws();

    sPendingChunks.clear();
    sPendingChunks.reserve(CHUNKS_PER_AXIS * CHUNKS_PER_AXIS * CHUNKS_PER_AXIS);
    for (int cy = 0; cy < CHUNKS_PER_AXIS; ++cy)
        for (int cz = 0; cz < CHUNKS_PER_AXIS; ++cz)
            for (int cx = 0; cx < CHUNKS_PER_AXIS; ++cx)
                sPendingChunks.push_back({cx, cy, cz});
}

static bool sWireframe = false;
static bool sWasComboDown = false;

static void onExit() {
    glDisable(GL_CULL_FACE);
}

static void onInput(float) {
    bool ctrlDown = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                 || glfwGetKey(ctx.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    bool oneDown  = glfwGetKey(ctx.window, GLFW_KEY_1) == GLFW_PRESS;
    bool combo = ctrlDown && oneDown;
    if (combo && !sWasComboDown) {
        sWireframe = !sWireframe;
        glPolygonMode(GL_FRONT_AND_BACK, sWireframe ? GL_LINE : GL_FILL);
    }
    sWasComboDown = combo;
}

static void onUpdate() {
    if (sPendingChunks.empty()) return;
    glm::vec3 camPos = getGlobalCamera()->position;

    for (int n = 0; n < CHUNKS_PER_FRAME && !sPendingChunks.empty(); ++n) {
        // Pick the pending chunk whose center is closest to the camera.
        auto closest = sPendingChunks.begin();
        float bestDist2 = std::numeric_limits<float>::max();
        for (auto it = sPendingChunks.begin(); it != sPendingChunks.end(); ++it) {
            glm::vec3 center = glm::vec3(*it) * (float)kChunkSize
                             + glm::vec3(kChunkSize * 0.5f);
            glm::vec3 d = center - camPos;
            float dist2 = glm::dot(d, d);
            if (dist2 < bestDist2) { bestDist2 = dist2; closest = it; }
        }
        glm::ivec3 chunk = *closest;
        // Swap-remove for O(1)
        *closest = sPendingChunks.back();
        sPendingChunks.pop_back();

        int x0 = chunk.x * kChunkSize, y0 = chunk.y * kChunkSize, z0 = chunk.z * kChunkSize;
        int x1 = std::min(x0 + kChunkSize, GRID);
        int y1 = std::min(y0 + kChunkSize, GRID);
        int z1 = std::min(z0 + kChunkSize, GRID);
        for (int z = z0; z < z1; ++z) {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float ns  = PerlinNoise::sample3D((float)x, (float)y, (float)z);
                    float ns2 = PerlinNoise::sample3D((float)x, (float)y, (float)z,
                                                      PerlinNoise::getSeed() + SECONDARY_SEED_OFFSET);
                    float threshold = (ns2 > SECONDARY_GATE) ? NOISE_THRESHOLD_LOOSE : NOISE_THRESHOLD;
                    float t = (float)y / (float)(GRID - 1);
                    threshold += HEIGHT_MOD_BOTTOM + t * (HEIGHT_MOD_TOP - HEIGHT_MOD_BOTTOM);
                    if (ns > threshold)
                        VE::draw("cube", (float)x, (float)y, (float)z);
                }
            }
        }
    }
}
static void onRender() {}

void registerVisualizerScene() {
    VE::registerScene("visualizer", onEnter, onExit, onInput, onUpdate, onRender);
}
