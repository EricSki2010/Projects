#include "noise2D.h"
#include "../VisualEngine/VisualEngine.h"
#include "../VisualEngine/EngineGlobals.h"
#include "perlinNoiseManagement.h"
#include "biome.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// 2D-noise heatmap visualizer.
//
// One Perlin sample per texel of an N×N RGB texture, each value mapped to a
// blue→cyan→green→yellow→red gradient. The texture is rendered onto a single
// flat quad with a top-down camera. No chunks, no streaming — purely static
// once built in onEnter.
// ─────────────────────────────────────────────────────────────────────────

static const int   HEATMAP_TEX_RES   = 512;    // texture resolution (pixels per side)
static const float HEATMAP_HALF_SPAN = 256.0f; // quad spans [-HALF_SPAN, +HALF_SPAN] in x and z
static const float HEATMAP_TEXEL_TO_WORLD = (HEATMAP_HALF_SPAN * 2.0f) / (float)HEATMAP_TEX_RES;

// Biome → display color. Classification logic + thresholds live in biome.cpp;
// this scene just maps the returned enum to pixels.
struct RGB { uint8_t r, g, b; };
static RGB colorFor(Biome b) {
    switch (b) {
        case Biome::Sand:     return {217, 178, 128};
        case Biome::Shrub:    return {102, 115,  51};
        case Biome::Forest:   return { 28, 102,  40};
        case Biome::Grass:    return { 51, 153,  51};
        case Biome::Rock:     return {128, 128, 128};
        case Biome::SnowRock: return {255, 255, 255};
    }
    return {255, 0, 255}; // magenta: unknown biome
}
// Keep the texture alive while the scene is active; Mesh stores a raw pointer.
static std::shared_ptr<Texture> sHeatmapTexture;
static std::shared_ptr<Mesh>    sHeatmapMesh;

static bool sWireframe = false;
static bool sWasComboDown = false;

// 5-stop heatmap gradient: blue → cyan → green → yellow → red.
static glm::vec3 heatmap(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    static const glm::vec3 stops[5] = {
        {0.05f, 0.10f, 0.55f}, // deep blue
        {0.10f, 0.55f, 0.85f}, // cyan
        {0.20f, 0.75f, 0.30f}, // green
        {0.95f, 0.85f, 0.20f}, // yellow
        {0.90f, 0.20f, 0.15f}, // red
    };
    float scaled = t * 4.0f;
    int i = (int)scaled;
    if (i >= 4) return stops[4];
    return glm::mix(stops[i], stops[i + 1], scaled - (float)i);
}

static std::vector<uint8_t> buildHeatmapPixels() {
    const int N = HEATMAP_TEX_RES;
    std::vector<uint8_t> pixels((size_t)N * N * 3);
    for (int v = 0; v < N; v++) {
        for (int u = 0; u < N; u++) {
            // Map texel center to world coords centered on origin.
            float wx = (-HEATMAP_HALF_SPAN) + ((float)u + 0.5f) * HEATMAP_TEXEL_TO_WORLD;
            float wz = (-HEATMAP_HALF_SPAN) + ((float)v + 0.5f) * HEATMAP_TEXEL_TO_WORLD;
            RGB c = colorFor(getBiome(wx, wz));
            size_t idx = (size_t)(v * N + u) * 3;
            pixels[idx + 0] = c.r;
            pixels[idx + 1] = c.g;
            pixels[idx + 2] = c.b;
        }
    }
    return pixels;
}

static void buildHeatmapMesh() {
    auto pixels = buildHeatmapPixels();
    sHeatmapTexture = std::make_shared<Texture>(pixels.data(),
                                                HEATMAP_TEX_RES, HEATMAP_TEX_RES, 3);

    // Single flat quad at y = 0, CCW winding viewed from +Y (so the front face
    // points up toward the bird's-eye camera).
    const float W = HEATMAP_HALF_SPAN;
    float verts[] = {
        // pos              // uv
        -W, 0.0f, -W,       0.0f, 0.0f,
         W, 0.0f, -W,       1.0f, 0.0f,
         W, 0.0f,  W,       1.0f, 1.0f,
        -W, 0.0f,  W,       0.0f, 1.0f,
    };
    unsigned int indices[] = { 0, 1, 2,  0, 2, 3 };

    sHeatmapMesh = std::make_shared<Mesh>(verts, 4, indices, 6);
    sHeatmapMesh->setTexture(sHeatmapTexture.get());
}

static void onEnter(std::shared_ptr<void>) {
    // Bird's-eye camera; pitch high enough for clear top-down feel without
    // hitting -90° (movement projects on horizontal forward, which would
    // become zero at exactly -90°).
    VE::setCamera(0.0f, 320.0f, 240.0f, 180.0f, -55.0f);
    VE::setMoveSpeed(60.0f);

    // Heatmap shouldn't be tinted by sky gradient or fog.
    VE::setGradientBackground(false);
    VE::setFog(false, glm::vec3(0.0f), 0.0f, 0.0f);
    VE::setFarPlane(1000.0f);
    VE::setBrightness(1.0f);
    VE::setSpecularStrength(0.0f);

    glDisable(GL_CULL_FACE); // visible from below too if you fly under

    buildHeatmapMesh();

    // Manage ctx.mergedMeshes ourselves — bypass the engine's auto-rebuild.
    ctx.mergedMeshes.clear();
    MergedMeshEntry e;
    e.mesh = sHeatmapMesh;
    e.texture = sHeatmapTexture;
    e.boundsCenter = glm::vec3(0.0f);
    e.boundsRadius = -1.0f; // skip distance/frustum cull (mesh is a single big quad)
    ctx.mergedMeshes.push_back(std::move(e));
    ctx.needsRebuild = false;
}

static void onExit() {
    ctx.mergedMeshes.clear();
    sHeatmapMesh.reset();
    sHeatmapTexture.reset();
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
}

static void onUpdate() {
    // Static heatmap — no per-frame work. Just keep auto-rebuild suppressed
    // so the engine doesn't wipe ctx.mergedMeshes.
    ctx.needsRebuild = false;
}
static void onRender() {}

void registerNoise2DScene() {
    VE::registerScene("noise2D", onEnter, onExit, onInput, onUpdate, onRender);
}
