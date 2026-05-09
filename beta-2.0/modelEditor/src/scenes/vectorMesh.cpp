#include "vectorMesh.h"
#include "../../../VisualEngine/VisualEngine.h"
#include "../../../VisualEngine/EngineGlobals.h"
#include "../../../VisualEngine/inputManagement/Camera.h"
#include "../../../VisualEngine/inputManagement/Raycasting.h"
#include "../../../VisualEngine/renderingManagement/primitives/LineRenderer.h"
#include "../../../VisualEngine/renderingManagement/primitives/DotRenderer.h"
#include "../../../VisualEngine/renderingManagement/meshing/Overlay.h"
#include "../../../VisualEngine/inputManagement/Collision.h"
#include "../../../VisualEngine/uiManagement/UIManager.h"
#include "../../../VisualEngine/uiManagement/UIRenderer.h"
#include "../../../VisualEngine/uiManagement/UIPrefabs.h"
#include "../../../VisualEngine/uiManagement/TextRenderer.h"
#include "../../../VisualEngine/uiManagement/EmbeddedFont.h"
#include "SceneData.h"
#include "../../../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../../../VisualEngine/memoryManagement/memory.h"
#include "../mechanics/AiHandling/AiHandling.h"
#include "../mechanics/AiHandling/AiTools.h"
#include "../../../VisualEngine/sceneManagement/SceneManager.h"
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <numeric>
#include <algorithm>
#include <random>

// 8 corners of a unit cube centered at origin
static const glm::vec3 cubeVerts[8] = {
    {-0.5f,  0.5f, -0.5f}, // 0 top back-left
    { 0.5f,  0.5f, -0.5f}, // 1 top back-right
    { 0.5f,  0.5f,  0.5f}, // 2 top front-right
    {-0.5f,  0.5f,  0.5f}, // 3 top front-left
    {-0.5f, -0.5f, -0.5f}, // 4 bottom back-left
    { 0.5f, -0.5f, -0.5f}, // 5 bottom back-right
    { 0.5f, -0.5f,  0.5f}, // 6 bottom front-right
    {-0.5f, -0.5f,  0.5f}, // 7 bottom front-left
};

// 12 edges as pairs of vertex indices
static const int cubeEdges[12][2] = {
    // Top square
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    // Bottom square
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    // Vertical connections
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static int sDrawMode = -1; // -1 = none, 0 = Vector, 1 = Line, 2 = Plane
static bool sHideOutlines = false;
static bool sWasLeftDown = false;
static bool sWasDDown = false;
static bool sWasADown = false;
static std::vector<glm::vec3> sPlacedDots;
static bool sHoverValid = false;
static glm::vec3 sHoverPoint;
static int sHoverPlacedIndex = -1;

// Line mode state
static std::vector<int> sLineSelectedDots; // indices into sPlacedDots, max 2
struct PlacedLine { int dotA; int dotB; };
static std::vector<PlacedLine> sPlacedLines;
static int sHoverLineIndex = -1;

// Plane mode state
static std::vector<int> sPlaneSelectedDots; // indices into sPlacedDots, max 3
struct PlacedTriangle { int dotA; int dotB; int dotC; bool flipped = false; };
static std::vector<PlacedTriangle> sPlacedTriangles;
static int sHoverTriIndex = -1;
static std::unordered_set<int> sSelectedTris;

// Vector mode: selected placed dot for live coordinate editing
static int sSelectedDotIndex = -1;
// One undo snapshot per coord-edit focus session, so typing keystrokes
// don't flood the stack.
static bool sCoordEditUndoPushed = false;

// File state
static std::string sVMeshName;
static int sReturnSlot = -1;
static std::string sReturnModelName;
static bool sVMeshPaused = false;
static bool sVMeshWasEscDown = false;
static bool sVMeshWasSpaceDown = false;
static bool sVMeshWasCtrlZDown = false;

// Undo system: snapshot of all placed geometry
struct VMeshSnapshot {
    std::vector<glm::vec3> dots;
    std::vector<PlacedLine> lines;
    std::vector<PlacedTriangle> triangles;
};
static std::vector<VMeshSnapshot> sVMeshUndoStack;
static const int kVMeshMaxUndoSteps = 50;

static void pushVMeshUndo() {
    VMeshSnapshot s;
    s.dots = sPlacedDots;
    s.lines = sPlacedLines;
    s.triangles = sPlacedTriangles;
    sVMeshUndoStack.push_back(std::move(s));
    if ((int)sVMeshUndoStack.size() > kVMeshMaxUndoSteps)
        sVMeshUndoStack.erase(sVMeshUndoStack.begin());
}

// Cardinal face helpers. All vectorMesh shapes live in a 1x1x1 box centered
// at origin, so side planes are at ±0.5 along each axis.
// Cardinal order: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
static int vmFaceNormalAxis(int f) { return f / 2; }
static bool vmFaceIsPositive(int f) { return (f & 1) == 0; }
static float vmFacePlaneCoord(int f) { return vmFaceIsPositive(f) ? 0.5f : -0.5f; }

static void exportVMeshToMesh() {
    if (sVMeshName.empty() || sPlacedTriangles.empty()) return;
    std::string meshPath = "assets/saves/vectorMeshes/" + sVMeshName + ".mesh";
    std::ofstream out(meshPath, std::ios::binary);
    if (!out) return;

    // Build vertex/index data and keep the per-triangle world-space verts
    // around so we can classify each triangle to a cardinal side afterward.
    struct WrittenTri { glm::vec3 va, vb, vc; };
    std::vector<WrittenTri> writtenTris;
    std::vector<float> verts;
    std::vector<unsigned int> indices;

    for (const auto& t : sPlacedTriangles) {
        if (t.dotA < (int)sPlacedDots.size() && t.dotB < (int)sPlacedDots.size() &&
            t.dotC < (int)sPlacedDots.size()) {

            glm::vec3 va = sPlacedDots[t.dotA];
            glm::vec3 vb = sPlacedDots[t.dotB];
            glm::vec3 vc = sPlacedDots[t.dotC];

            // Blanket flip on save (user's natural click is CW from their view).
            // Per-triangle Flip Normal overrides via t.flipped.
            glm::vec3 normal = glm::cross(vb - va, vc - va);
            if (t.flipped) {
                std::swap(vb, vc);
                normal = -normal;
            }
            normal = glm::normalize(-normal);

            unsigned int base = (unsigned int)(verts.size() / 8);
            auto addVert = [&](const glm::vec3& p, const glm::vec3& n) {
                verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
                verts.push_back(0.0f); verts.push_back(0.0f);
                verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
            };

            addVert(va, normal); addVert(vb, normal); addVert(vc, normal);
            indices.push_back(base); indices.push_back(base+1); indices.push_back(base+2);

            writtenTris.push_back({va, vb, vc});
        }
    }

    // ── Face cull state computation ────────────────────────────────────
    //
    // 1. Classify each triangle: if all 3 verts lie on a cardinal side plane
    //    (±0.5 along one axis) and within the 1x1 face bounds, tag with that
    //    side (0..5). Otherwise leave at -1 for now.
    const float FACE_EPS = 0.01f;
    int32_t triCount = (int32_t)writtenTris.size();
    std::vector<int32_t> triFaceDir(triCount, -1);

    for (int i = 0; i < triCount; i++) {
        const auto& t = writtenTris[i];
        for (int f = 0; f < 6; f++) {
            int axis = vmFaceNormalAxis(f);
            float plane = vmFacePlaneCoord(f);
            bool allOnPlane =
                std::fabs(t.va[axis] - plane) < FACE_EPS &&
                std::fabs(t.vb[axis] - plane) < FACE_EPS &&
                std::fabs(t.vc[axis] - plane) < FACE_EPS;
            if (!allOnPlane) continue;

            int a1 = (axis + 1) % 3;
            int a2 = (axis + 2) % 3;
            auto inSquare = [&](const glm::vec3& v) {
                return v[a1] >= -0.5f - FACE_EPS && v[a1] <= 0.5f + FACE_EPS
                    && v[a2] >= -0.5f - FACE_EPS && v[a2] <= 0.5f + FACE_EPS;
            };
            if (!inSquare(t.va) || !inSquare(t.vb) || !inSquare(t.vc)) continue;

            triFaceDir[i] = f;
            break;
        }
    }

    // 2. Sum projected areas of flat triangles per side. Full 1.0 area means
    //    the side is fully covered (state 2). Any area means partially
    //    covered (state 1). None means open (state 0).
    int32_t faceState[6] = {0, 0, 0, 0, 0, 0};
    float faceArea[6] = {0.0f};
    for (int i = 0; i < triCount; i++) {
        int f = triFaceDir[i];
        if (f < 0) continue;
        const auto& t = writtenTris[i];
        int axis = vmFaceNormalAxis(f);
        int a1 = (axis + 1) % 3;
        int a2 = (axis + 2) % 3;
        float e1x = t.vb[a1] - t.va[a1];
        float e1y = t.vb[a2] - t.va[a2];
        float e2x = t.vc[a1] - t.va[a1];
        float e2y = t.vc[a2] - t.va[a2];
        faceArea[f] += 0.5f * std::fabs(e1x * e2y - e2x * e1y);
    }
    for (int f = 0; f < 6; f++) {
        if (faceArea[f] >= 0.99f)    faceState[f] = 2;
        else if (faceArea[f] > 0.0f) faceState[f] = 1;
        else                         faceState[f] = 0;
    }

    // 3. Non-flat triangles (still -1): tag with nearest cardinal side whose
    //    state is 0. Sides at state 1 or 2 are already "owned" by flat
    //    triangles; interior / slanted geometry shouldn't be culled along
    //    with them. If no open side exists, stays -1 (never culled).
    for (int i = 0; i < triCount; i++) {
        if (triFaceDir[i] >= 0) continue;
        const auto& t = writtenTris[i];
        glm::vec3 centroid = (t.va + t.vb + t.vc) / 3.0f;
        int bestFace = -1;
        float bestDist = std::numeric_limits<float>::max();
        for (int f = 0; f < 6; f++) {
            if (faceState[f] != 0) continue;
            int axis = vmFaceNormalAxis(f);
            float dist = std::fabs(centroid[axis] - vmFacePlaneCoord(f));
            if (dist < bestDist) { bestDist = dist; bestFace = f; }
        }
        triFaceDir[i] = bestFace;
    }

    // ── Write the .mesh file (VC format = VN + appended culling data) ──
    uint32_t vertCount = (uint32_t)(verts.size() / 8);
    uint32_t idxCount = (uint32_t)indices.size();
    uint32_t texPathLen = 0;
    char magic[2] = {'V', 'C'};
    out.write(magic, 2);
    out.write(reinterpret_cast<const char*>(&vertCount), 4);
    out.write(reinterpret_cast<const char*>(&idxCount), 4);
    out.write(reinterpret_cast<const char*>(&texPathLen), 4);
    out.write(reinterpret_cast<const char*>(verts.data()), verts.size() * sizeof(float));
    out.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint32_t));
    // Culling data: triFaceDir[triCount] as int32, faceState[6] as int32.
    out.write(reinterpret_cast<const char*>(triFaceDir.data()), triCount * sizeof(int32_t));
    out.write(reinterpret_cast<const char*>(faceState), 6 * sizeof(int32_t));
}

static void saveVectorMesh() {
    if (sVMeshName.empty()) return;
    setMemoryPath("assets/saves/vectorMeshes");
    std::string path = "assets/saves/vectorMeshes/" + sVMeshName + ".vmesh";
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cerr << "Failed to save: " << path << std::endl; return; }

    // Write dots
    uint32_t dotCount = (uint32_t)sPlacedDots.size();
    out.write(reinterpret_cast<const char*>(&dotCount), 4);
    for (const auto& d : sPlacedDots)
        out.write(reinterpret_cast<const char*>(&d), sizeof(glm::vec3));

    // Write lines
    uint32_t lineCount = (uint32_t)sPlacedLines.size();
    out.write(reinterpret_cast<const char*>(&lineCount), 4);
    for (const auto& l : sPlacedLines) {
        out.write(reinterpret_cast<const char*>(&l.dotA), 4);
        out.write(reinterpret_cast<const char*>(&l.dotB), 4);
    }

    // Write triangles
    uint32_t triCount = (uint32_t)sPlacedTriangles.size();
    out.write(reinterpret_cast<const char*>(&triCount), 4);
    for (const auto& t : sPlacedTriangles) {
        out.write(reinterpret_cast<const char*>(&t.dotA), 4);
        out.write(reinterpret_cast<const char*>(&t.dotB), 4);
        out.write(reinterpret_cast<const char*>(&t.dotC), 4);
        uint8_t flipped = t.flipped ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&flipped), 1);
    }
}

static void loadVectorMesh(const std::string& name) {
    sVMeshName = name;
    sPlacedDots.clear();
    sPlacedLines.clear();
    sPlacedTriangles.clear();

    std::string path = "assets/saves/vectorMeshes/" + name + ".vmesh";
    std::ifstream in(path, std::ios::binary);
    if (!in) return; // new file, start empty

    // Read dots
    uint32_t dotCount = 0;
    in.read(reinterpret_cast<char*>(&dotCount), 4);
    sPlacedDots.resize(dotCount);
    for (uint32_t i = 0; i < dotCount; i++)
        in.read(reinterpret_cast<char*>(&sPlacedDots[i]), sizeof(glm::vec3));

    // Read lines
    uint32_t lineCount = 0;
    in.read(reinterpret_cast<char*>(&lineCount), 4);
    sPlacedLines.resize(lineCount);
    for (uint32_t i = 0; i < lineCount; i++) {
        in.read(reinterpret_cast<char*>(&sPlacedLines[i].dotA), 4);
        in.read(reinterpret_cast<char*>(&sPlacedLines[i].dotB), 4);
    }

    // Read triangles
    uint32_t triCount = 0;
    in.read(reinterpret_cast<char*>(&triCount), 4);
    sPlacedTriangles.resize(triCount);
    for (uint32_t i = 0; i < triCount; i++) {
        in.read(reinterpret_cast<char*>(&sPlacedTriangles[i].dotA), 4);
        in.read(reinterpret_cast<char*>(&sPlacedTriangles[i].dotB), 4);
        in.read(reinterpret_cast<char*>(&sPlacedTriangles[i].dotC), 4);
        uint8_t flipped = 0;
        in.read(reinterpret_cast<char*>(&flipped), 1);
        sPlacedTriangles[i].flipped = (flipped != 0);
    }
}

static void openVMeshPauseMenu() {
    sVMeshPaused = true;

    Camera* cam = getGlobalCamera();
    if (cam->looking) {
        cam->looking = false;
        glfwSetInputMode(ctx.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    addUIGroup("vmesh_pause");

    float btnW = 0.3f;
    float btnH = 0.08f;
    float btnX = -btnW / 2.0f;
    glm::vec4 btnColor = {0.15f, 0.15f, 0.15f, 0.95f};

    addToGroup("vmesh_pause", createButton("vm_continue",
        btnX, 0.1f, btnW, btnH, btnColor, "Continue",
        []() {
            sVMeshPaused = false;
            VE::setBrightness(1.0f);
            removeUIGroup("vmesh_pause");
        }
    ));

    addToGroup("vmesh_pause", createButton("vm_save_exit",
        btnX, -0.02f, btnW, btnH,
        {0.1f, 0.4f, 0.15f, 0.95f}, "Save & Exit",
        []() {
            saveVectorMesh();
            exportVMeshToMesh();
            sVMeshPaused = false;
            VE::setBrightness(1.0f);
            removeUIGroup("vmesh_pause");
            if (!sReturnModelName.empty())
                VE::setScene("3dModeler", std::make_shared<std::string>(sReturnModelName));
            else
                VE::setScene("menu");
        }
    ));
}

// Build all snap points from edges, deduplicating corners
static std::vector<glm::vec3> buildSnapPoints(int snapCount) {
    std::vector<glm::vec3> points;

    // Always include corners (endpoints)
    for (int i = 0; i < 8; i++)
        points.push_back(cubeVerts[i]);

    // Include placed dot positions as snap points
    for (const auto& d : sPlacedDots)
        points.push_back(d);

    // Add intermediate points along each wireframe edge
    int divisions = snapCount > 0 ? snapCount + 1 : 0;
    if (divisions > 0) {
        for (int e = 0; e < 12; e++) {
            glm::vec3 from = cubeVerts[cubeEdges[e][0]];
            glm::vec3 to = cubeVerts[cubeEdges[e][1]];
            for (int i = 1; i < divisions; i++) {
                float t = (float)i / (float)divisions;
                points.push_back(from + t * (to - from));
            }
        }

        // Add intermediate points along placed lines
        for (const auto& l : sPlacedLines) {
            if (l.dotA < (int)sPlacedDots.size() && l.dotB < (int)sPlacedDots.size()) {
                glm::vec3 from = sPlacedDots[l.dotA];
                glm::vec3 to = sPlacedDots[l.dotB];
                for (int i = 1; i < divisions; i++) {
                    float t = (float)i / (float)divisions;
                    points.push_back(from + t * (to - from));
                }
            }
        }
    }

    // Deduplicate (corners shared by multiple edges)
    std::vector<glm::vec3> unique;
    for (const auto& p : points) {
        bool dup = false;
        for (const auto& u : unique)
            if (glm::length(p - u) < 0.001f) { dup = true; break; }
        if (!dup) unique.push_back(p);
    }
    return unique;
}

// Find the nearest snap point to a 3D position
static int findNearestSnap(const std::vector<glm::vec3>& snaps, const glm::vec3& pos) {
    int best = -1;
    float bestDist = 999999.0f;
    for (int i = 0; i < (int)snaps.size(); i++) {
        float d = glm::length(snaps[i] - pos);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// Find if mouse is near a placed dot (screen-space check, closest to camera wins)
static int findHoveredDot(double mx, double my) {
    int best = -1;
    float bestDepth = 999999.0f;
    for (int i = 0; i < (int)sPlacedDots.size(); i++) {
        glm::vec4 clip = ctx.scene->projection * ctx.scene->view * glm::vec4(sPlacedDots[i], 1.0f);
        if (clip.w <= 0.0001f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = (ndc.x * 0.5f + 0.5f) * ctx.width;
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * ctx.height;
        float dist = glm::length(glm::vec2(sx, sy) - glm::vec2((float)mx, (float)my));
        float depth = clip.w; // distance from camera
        if (dist < 12.0f && depth < bestDepth) {
            bestDepth = depth;
            best = i;
        }
    }
    return best;
}

static const float PANEL_X = 0.6f;
static const float PANEL_W = 0.4f;
static const float PANEL_PAD = 0.02f;
static const float SIDE_BTN_W = PANEL_W - PANEL_PAD * 2;
static const float SIDE_BTN_H = 0.06f;
static const float SIDE_BTN_X = PANEL_X + PANEL_PAD;
static const float SIDE_START_Y = 0.9f;
static const float SIDE_GAP = SIDE_BTN_H + PANEL_PAD;

// Subdivide each triangle in `indices` into 4 sub-triangles by connecting
// edge midpoints. Reuses any existing vertex within 1e-4 of a midpoint.
// The original triangle is replaced in-place at its old index (so existing
// references stay valid), and 3 new triangles are appended per subdivision.
// Returns the number of triangles subdivided. Caller pushes undo.
static int subdivideTriangleIndices(std::vector<int> indices) {
    if (indices.empty()) return 0;
    // Sort descending + dedup so we don't subdivide the same triangle twice.
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    auto findOrAddVert = [](const glm::vec3& p) -> int {
        const float eps = 1e-4f;
        for (int i = 0; i < (int)sPlacedDots.size(); i++) {
            if (glm::distance(sPlacedDots[i], p) < eps) return i;
        }
        sPlacedDots.push_back(p);
        return (int)sPlacedDots.size() - 1;
    };

    int subdivided = 0;
    for (int idx : indices) {
        if (idx < 0 || idx >= (int)sPlacedTriangles.size()) continue;
        PlacedTriangle t = sPlacedTriangles[idx];
        glm::vec3 A = sPlacedDots[t.dotA];
        glm::vec3 B = sPlacedDots[t.dotB];
        glm::vec3 C = sPlacedDots[t.dotC];
        int abIdx = findOrAddVert((A + B) * 0.5f);
        int bcIdx = findOrAddVert((B + C) * 0.5f);
        int caIdx = findOrAddVert((C + A) * 0.5f);
        sPlacedTriangles[idx] = {t.dotA, abIdx, caIdx, t.flipped};
        sPlacedTriangles.push_back({abIdx, t.dotB, bcIdx, t.flipped});
        sPlacedTriangles.push_back({caIdx, bcIdx, t.dotC, t.flipped});
        sPlacedTriangles.push_back({abIdx, bcIdx, caIdx, t.flipped});
        subdivided++;
    }
    return subdivided;
}

static void rebuildModeTools() {
    removeUIGroup("mode_tools");
    addUIGroup("mode_tools");

    // Start below: hide toggle at SIDE_START_Y, dropdown at -1 gap, tools at -2 gaps
    float y = SIDE_START_Y - SIDE_GAP * 2;

    if (sDrawMode == 0) { // Vector
        addToGroup("mode_tools", createButton("snap_label",
            SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
            {0.0f, 0.0f, 0.0f, 0.0f}, "Snap Point Count",
            nullptr
        ));
        y -= SIDE_GAP;

        UIElement snapInput = createTextInput("snap_input",
            SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
            {0.18f, 0.18f, 0.18f, 0.95f},
            "Enter value...", 3);
        snapInput.numericOnly = true;
        addToGroup("mode_tools", snapInput);
        y -= SIDE_GAP;
    } else if (sDrawMode == 1) { // Line
        // Line mode tools can go here
    } else if (sDrawMode == 2) { // Plane
        if (!sSelectedTris.empty()) {
            std::string flipLabel = sSelectedTris.size() > 1
                ? "Flip Normals (" + std::to_string(sSelectedTris.size()) + ")"
                : "Flip Normal";
            addToGroup("mode_tools", createButton("flip_normal",
                SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
                {0.3f, 0.2f, 0.5f, 0.95f}, flipLabel,
                []() {
                    if (sSelectedTris.empty()) return;
                    pushVMeshUndo();
                    int n = (int)sPlacedTriangles.size();
                    for (int i : sSelectedTris) {
                        if (i >= 0 && i < n)
                            sPlacedTriangles[i].flipped = !sPlacedTriangles[i].flipped;
                    }
                }
            ));
            y -= SIDE_GAP;

            std::string subLabel = sSelectedTris.size() > 1
                ? "Subdivide (" + std::to_string(sSelectedTris.size()) + ")"
                : "Subdivide";
            addToGroup("mode_tools", createButton("subdivide",
                SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
                {0.2f, 0.4f, 0.3f, 0.95f}, subLabel,
                []() {
                    if (sSelectedTris.empty()) return;
                    std::vector<int> indices(sSelectedTris.begin(), sSelectedTris.end());
                    pushVMeshUndo();
                    int oldCount = (int)sPlacedTriangles.size();
                    int subdivided = subdivideTriangleIndices(indices);
                    // Originals stay at their old indices; 3 new appended per
                    // subdivision. Extend selection to include them so the
                    // user can keep mashing Subdivide.
                    for (int i = 0; i < 3 * subdivided; i++)
                        sSelectedTris.insert(oldCount + i);
                    rebuildModeTools();
                }
            ));
            y -= SIDE_GAP;
        }
    }
}

// Delete every triangle currently in sSelectedTris. Indices stay consistent
// because we filter into a kept-vector rather than erasing in place.
static void deleteSelectedTriangles() {
    if (sSelectedTris.empty()) return;
    pushVMeshUndo();
    std::vector<PlacedTriangle> kept;
    kept.reserve(sPlacedTriangles.size() - sSelectedTris.size());
    for (int i = 0; i < (int)sPlacedTriangles.size(); i++) {
        if (!sSelectedTris.count(i)) kept.push_back(sPlacedTriangles[i]);
    }
    sPlacedTriangles = std::move(kept);
    sSelectedTris.clear();
    sHoverTriIndex = -1;
    rebuildModeTools();
}

// Builds (or rebuilds) the vector-mode "Selected Vertex" sidebar group.
// Called on selection change to refresh the X/Y/Z input prefills.
static void showVectorSelectedTools() {
    removeUIGroup("vector_selected_tools");
    if (sSelectedDotIndex < 0 || sSelectedDotIndex >= (int)sPlacedDots.size()) return;

    addUIGroup("vector_selected_tools");
    glm::vec3 v = sPlacedDots[sSelectedDotIndex];
    auto fmt = [](float f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", f);
        return std::string(buf);
    };

    // Sit below mode_tools (snap label + snap input use 2 slots after the
    // dropdown). Add one extra gap of breathing room.
    float y = SIDE_START_Y - SIDE_GAP * 5;

    UIElement header = createButton("vsel_header",
        SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
        {0.0f, 0.0f, 0.0f, 0.0f}, "Selected Vertex", nullptr);
    addToGroup("vector_selected_tools", header);
    y -= SIDE_GAP;

    glm::vec4 inputColor = {0.18f, 0.18f, 0.18f, 0.95f};
    auto addCoord = [&](const std::string& id, const std::string& placeholder, float val) {
        UIElement input = createTextInput(id,
            SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
            inputColor, placeholder, 16);
        input.inputText = fmt(val);
        input.numericOnly = true;
        addToGroup("vector_selected_tools", input);
        y -= SIDE_GAP;
    };
    addCoord("vsel_x", "X", v.x);
    addCoord("vsel_y", "Y", v.y);
    addCoord("vsel_z", "Z", v.z);

    addToGroup("vector_selected_tools", createButton("vsel_deselect",
        SIDE_BTN_X, y, SIDE_BTN_W, SIDE_BTN_H,
        {0.25f, 0.25f, 0.25f, 0.95f}, "Deselect",
        []() {
            sSelectedDotIndex = -1;
            sCoordEditUndoPushed = false;
            removeUIGroup("vector_selected_tools");
        }
    ));
}

static void hideVectorSelectedTools() {
    sSelectedDotIndex = -1;
    sCoordEditUndoPushed = false;
    removeUIGroup("vector_selected_tools");
}

// ---------------------------------------------------------------------------
// Helpers shared between AI tools (af / fn / build).
// ---------------------------------------------------------------------------

// Groups triangles into connected components (shared-edge flood fill),
// orients each triangle's normal outward from its component's vertex centroid,
// and reports whether every component was airtight (every edge shared by
// exactly 2 triangles).
static bool aiAutoFixNormals() {
    auto edgeKey = [](int u, int v) -> uint64_t {
        if (u > v) std::swap(u, v);
        return ((uint64_t)(uint32_t)u << 32) | (uint32_t)v;
    };
    const int triCount = (int)sPlacedTriangles.size();
    if (triCount == 0) return true;

    std::vector<int> parent(triCount);
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb) parent[ra] = rb;
    };

    std::unordered_map<uint64_t, std::vector<int>> edgeToTris;
    for (int i = 0; i < triCount; i++) {
        const auto& t = sPlacedTriangles[i];
        edgeToTris[edgeKey(t.dotA, t.dotB)].push_back(i);
        edgeToTris[edgeKey(t.dotB, t.dotC)].push_back(i);
        edgeToTris[edgeKey(t.dotC, t.dotA)].push_back(i);
    }
    for (auto& kv : edgeToTris) {
        const auto& tris = kv.second;
        for (size_t k = 1; k < tris.size(); k++) unite(tris[0], tris[k]);
    }

    std::unordered_map<int, std::vector<int>> components;
    for (int i = 0; i < triCount; i++) components[find(i)].push_back(i);

    bool allAirtight = true;
    for (auto& kv : components) {
        const auto& tris = kv.second;
        std::unordered_set<int> vertSet;
        for (int ti : tris) {
            const auto& t = sPlacedTriangles[ti];
            vertSet.insert(t.dotA); vertSet.insert(t.dotB); vertSet.insert(t.dotC);
        }
        glm::vec3 centroid(0.0f);
        for (int vi : vertSet) centroid += sPlacedDots[vi];
        if (!vertSet.empty()) centroid /= (float)vertSet.size();

        std::unordered_map<uint64_t, int> localEdges;
        for (int ti : tris) {
            const auto& t = sPlacedTriangles[ti];
            localEdges[edgeKey(t.dotA, t.dotB)]++;
            localEdges[edgeKey(t.dotB, t.dotC)]++;
            localEdges[edgeKey(t.dotC, t.dotA)]++;
        }
        for (auto& ek : localEdges)
            if (ek.second != 2) { allAirtight = false; break; }

        for (int ti : tris) {
            const auto& t = sPlacedTriangles[ti];
            glm::vec3 A = sPlacedDots[t.dotA];
            glm::vec3 B = sPlacedDots[t.dotB];
            glm::vec3 C = sPlacedDots[t.dotC];
            glm::vec3 n = glm::cross(C - A, B - A);
            if (t.flipped) n = -n;
            glm::vec3 triCentroid = (A + B + C) / 3.0f;
            if (glm::dot(n, triCentroid - centroid) < 0.0f)
                sPlacedTriangles[ti].flipped = !sPlacedTriangles[ti].flipped;
        }
    }
    return allAirtight;
}

// Saves and exports the current vmesh, then transitions back to 3dModeler
// (or menu if there is no return context). Mirrors the space-release flow.
static void aiFinishVMeshEdit() {
    saveVectorMesh();
    exportVMeshToMesh();
    sVMeshPaused = false;
    VE::setBrightness(1.0f);
    removeUIGroup("vmesh_pause");
    if (!sReturnModelName.empty())
        VE::setScene("3dModeler", std::make_shared<std::string>(sReturnModelName));
    else
        VE::setScene("menu");
}

static int getSnapCount() {
    std::string val = getInputText("mode_tools", "snap_input");
    if (val.empty()) return 0;
    try { return std::stoi(val); }
    catch (...) { return 0; }
}

void registerVectorMeshScene() {
    VE::registerScene("vectorMesh",
        // onEnter
        [](std::shared_ptr<void> data) {
            getGlobalCamera()->setMode(CAMERA_FPS);
            initLineRenderer();
            initDotRenderer();
            initOverlay();
            initUIRenderer();
            initTextRendererFromMemory(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE, 48);
            VE::setCamera(2, 1.5f, 2, 210, -25);
            VE::setGradientBackground(true);
            sDrawMode = -1;
            sPlacedDots.clear();
            sPlacedLines.clear();
            sPlacedTriangles.clear();
            sWasLeftDown = false;
            sVMeshPaused = false;
            sVMeshWasEscDown = false;
            sSelectedTris.clear();
            sHoverTriIndex = -1;
            sSelectedDotIndex = -1;
            sCoordEditUndoPushed = false;

            sReturnSlot = -1;
            sReturnModelName = "";

            // Load vector mesh if data was passed
            if (data) {
                auto editData = std::static_pointer_cast<VectorMeshEditData>(data);
                loadVectorMesh(editData->meshName);
                sReturnSlot = editData->slotIndex;
                sReturnModelName = editData->modelName;
            }
            sVMeshUndoStack.clear();

            // Right sidebar
            addUIGroup("sidebar");
            addToGroup("sidebar", createPanel("sidebar_bg",
                PANEL_X, -1.0f, PANEL_W, 2.0f,
                {0.12f, 0.12f, 0.12f, 0.85f}
            ));

            // Hide outlines toggle
            addToGroup("sidebar", createButton("hide_outlines",
                SIDE_BTN_X, SIDE_START_Y, SIDE_BTN_W, SIDE_BTN_H,
                {0.25f, 0.25f, 0.25f, 0.95f}, "Hide Outlines: Off",
                []() {
                    sHideOutlines = !sHideOutlines;
                    UIElement* btn = getUIElement("sidebar", "hide_outlines");
                    if (btn) btn->label = sHideOutlines ? "Hide Outlines: On" : "Hide Outlines: Off";
                }
            ));

            // Draw mode dropdown
            createDropdown("sidebar", "draw_mode",
                SIDE_BTN_X, SIDE_START_Y - SIDE_GAP, SIDE_BTN_W, SIDE_BTN_H,
                {0.18f, 0.18f, 0.18f, 0.95f},
                "Draw Mode...",
                {"Vector", "Line", "Plane"},
                [](int index, const std::string& option) {
                    sDrawMode = index;
                    sLineSelectedDots.clear();
                    sPlaneSelectedDots.clear();
                    sSelectedTris.clear();
                    hideVectorSelectedTools();
                    rebuildModeTools();
                },
                0.0f, -SIDE_GAP
            );

            // Left-side AI panel (mirror of 3dModeler's layout).
            if (AI::isEnabled()) {
                float aiPanelX = -1.0f;
                float aiPanelW = 0.4f;
                float aiPad = 0.02f;
                float aiBtnW = aiPanelW - aiPad * 2;
                float aiBtnX = aiPanelX + aiPad;
                float aiBtnH = 0.06f;

                addUIGroup("ai_panel");
                addToGroup("ai_panel", createPanel("ai_bg",
                    aiPanelX, -1.0f, aiPanelW, 2.0f,
                    {0.12f, 0.12f, 0.12f, 0.85f}
                ));

                const float aiRespBottom = 0.50f;
                const float aiRespHeight = 0.45f;
                const int   aiRespLines  = 8;
                const float aiLineH = aiRespHeight / aiRespLines;

                addToGroup("ai_panel", createPanel("ai_response",
                    aiBtnX, aiRespBottom, aiBtnW, aiRespHeight,
                    {0.16f, 0.16f, 0.20f, 0.85f}
                ));
                for (int i = 0; i < aiRespLines; i++) {
                    UIElement line = createPanel(
                        "ai_line_" + std::to_string(i),
                        aiBtnX,
                        aiRespBottom + aiRespHeight - (i + 1) * aiLineH,
                        aiBtnW, aiLineH,
                        {0.0f, 0.0f, 0.0f, 0.0f}
                    );
                    line.labelScale = 0.25f;
                    line.labelColor = {0.88f, 0.88f, 0.88f, 1.0f};
                    line.label = (i == 0) ? "Ready" : "";
                    addToGroup("ai_panel", line);
                }

                UIElement promptEl = createTextInput("ai_prompt",
                    aiBtnX, 0.42f, aiBtnW, aiBtnH,
                    {0.18f, 0.18f, 0.22f, 0.95f},
                    "Ask AI...", 200);
                // Intentionally no onUnfocus: clicking away just unfocuses.
                // Submission happens only via the Send button.
                promptEl.multiline = true;
                addToGroup("ai_panel", promptEl);

                addToGroup("ai_panel", createButton("ai_send",
                    aiBtnX, 0.34f, aiBtnW, aiBtnH,
                    {0.25f, 0.40f, 0.60f, 0.95f}, "Send",
                    []() {
                        std::string text = getInputText("ai_panel", "ai_prompt");
                        if (text.empty()) return;
                        AI::submitPrompt(text);
                        if (auto* in = getUIElement("ai_panel", "ai_prompt"))
                            in->inputText.clear();
                    }
                ));
            }
        },
        // onExit
        []() {
            cleanupLineRenderer();
            cleanupDotRenderer();
            cleanupOverlay();
            VE::setBrightness(1.0f);
            VE::setGradientBackground(false);
            cleanupTextRenderer();
            cleanupUIRenderer();
            clearUI();
        },
        // onInput
        [](float dt) {
            AI::update(dt);

            // Grow the AI prompt vertically to fit its current text; reposition
            // Send below it. Top edge stays anchored at 0.48.
            if (auto* input = getUIElement("ai_panel", "ai_prompt")) {
                const float kBaseH = 0.06f;
                const float kLineGrow = 0.05f;
                const float kTopEdge = 0.48f;
                int lines = inputWrappedLineCount(*input);
                float newH = kBaseH + kLineGrow * (float)std::max(0, lines - 1);
                input->size.y = newH;
                input->position.y = kTopEdge - newH;
                if (auto* send = getUIElement("ai_panel", "ai_send")) {
                    send->position.y = input->position.y - 0.02f - 0.06f;
                }
            }

            // Mirror AI status/response into the left panel's 8-row text area,
            // word-wrapping to fit the panel width.
            {
                std::string response = AI::getLastResponse();
                std::string display = !response.empty() ? response : AI::getStatus();

                const size_t kCharsPerLine = 26;
                const int kRows = 8;
                std::vector<std::string> lines;
                std::string cur, word;
                auto flushWord = [&]() {
                    if (word.empty()) return;
                    if (cur.empty()) cur = word;
                    else if (cur.size() + 1 + word.size() <= kCharsPerLine) cur += " " + word;
                    else { lines.push_back(cur); cur = word; }
                    word.clear();
                };
                for (char c : display) {
                    if (c == '\n') { flushWord(); if (!cur.empty()) { lines.push_back(cur); cur.clear(); } }
                    else if (c == ' ' || c == '\t') { flushWord(); }
                    else {
                        word += c;
                        if (word.size() > kCharsPerLine) {
                            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
                            lines.push_back(word.substr(0, kCharsPerLine));
                            word = word.substr(kCharsPerLine);
                        }
                    }
                }
                flushWord();
                if (!cur.empty()) lines.push_back(cur);
                if ((int)lines.size() > kRows) {
                    lines.resize(kRows);
                    std::string& last = lines.back();
                    if (last.size() > kCharsPerLine - 3) last.resize(kCharsPerLine - 3);
                    last += "...";
                }
                for (int i = 0; i < kRows; i++) {
                    std::string id = "ai_line_" + std::to_string(i);
                    if (auto* el = getUIElement("ai_panel", id))
                        el->label = (i < (int)lines.size()) ? lines[i] : "";
                }
            }

            // Esc toggle pause menu
            bool escDown = glfwGetKey(ctx.window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escDown && !sVMeshWasEscDown) {
                if (sVMeshPaused) {
                    sVMeshPaused = false;
                    VE::setBrightness(1.0f);
                    removeUIGroup("vmesh_pause");
                } else {
                    openVMeshPauseMenu();
                }
            }
            sVMeshWasEscDown = escDown;

            // Space: save and exit back to 3dModeler - fires on release.
            // Ignored while in camera movement mode so space can be used for jump/up.
            Camera* spaceCam = getGlobalCamera();
            bool spaceInMovement = spaceCam && spaceCam->looking;
            bool spaceDown = !spaceInMovement && !isAnyInputFocused()
                             && glfwGetKey(ctx.window, GLFW_KEY_SPACE) == GLFW_PRESS;
            if (!spaceDown && sVMeshWasSpaceDown) {
                sVMeshWasSpaceDown = false;
                saveVectorMesh();
                exportVMeshToMesh();
                sVMeshPaused = false;
                VE::setBrightness(1.0f);
                removeUIGroup("vmesh_pause");
                if (!sReturnModelName.empty())
                    VE::setScene("3dModeler", std::make_shared<std::string>(sReturnModelName));
                else
                    VE::setScene("menu");
                return;
            }
            sVMeshWasSpaceDown = spaceDown;

            // Ctrl+Z: undo
            bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
            bool zDown = glfwGetKey(ctx.window, GLFW_KEY_Z) == GLFW_PRESS;
            bool ctrlZ = ctrlHeld && zDown;
            if (ctrlZ && !sVMeshWasCtrlZDown) {
                if (!sVMeshUndoStack.empty()) {
                    VMeshSnapshot s = sVMeshUndoStack.back();
                    sVMeshUndoStack.pop_back();
                    sPlacedDots = s.dots;
                    sPlacedLines = s.lines;
                    sPlacedTriangles = s.triangles;
                    sLineSelectedDots.clear();
                    sPlaneSelectedDots.clear();
                    sSelectedTris.clear();
                    hideVectorSelectedTools();
                    sHoverPlacedIndex = -1;
                    sHoverLineIndex = -1;
                    sHoverTriIndex = -1;
                    rebuildModeTools();
                }
            }
            sVMeshWasCtrlZDown = ctrlZ;

            processUIInput();

            // Live coord-edit: parse the X/Y/Z inputs and apply to the
            // selected vertex. Undo gets one snapshot per focus session.
            if (sDrawMode == 0 && sSelectedDotIndex >= 0 &&
                sSelectedDotIndex < (int)sPlacedDots.size()) {
                auto parseOr = [](const std::string& s, float fallback) {
                    if (s.empty() || s == "-" || s == "." || s == "-.") return fallback;
                    try { return std::stof(s); } catch (...) { return fallback; }
                };
                glm::vec3 cur = sPlacedDots[sSelectedDotIndex];
                std::string sx = getInputText("vector_selected_tools", "vsel_x");
                std::string sy = getInputText("vector_selected_tools", "vsel_y");
                std::string sz = getInputText("vector_selected_tools", "vsel_z");
                glm::vec3 next(parseOr(sx, cur.x), parseOr(sy, cur.y), parseOr(sz, cur.z));

                bool anyFocused = false;
                for (const char* id : {"vsel_x", "vsel_y", "vsel_z"}) {
                    UIElement* el = getUIElement("vector_selected_tools", id);
                    if (el && el->focused) { anyFocused = true; break; }
                }
                if (!anyFocused) sCoordEditUndoPushed = false;

                if (next != cur) {
                    if (!sCoordEditUndoPushed) {
                        pushVMeshUndo();
                        sCoordEditUndoPushed = true;
                    }
                    sPlacedDots[sSelectedDotIndex] = next;
                }
            } else {
                sCoordEditUndoPushed = false;
            }

            if (sVMeshPaused) return;

            if (!getGlobalCamera()->looking) {
                double mx, my;
                glfwGetCursorPos(ctx.window, &mx, &my);

                sHoverValid = false;
                sHoverPlacedIndex = -1;

                // Only interact outside sidebar, and only in Vector mode
                if (mx < ctx.width * 0.8f && sDrawMode == 0) {
                    // Check if hovering over a placed dot
                    sHoverPlacedIndex = findHoveredDot(mx, my);

                    // Ctrl+D to delete hovered placed dot
                    bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    bool dDown = glfwGetKey(ctx.window, GLFW_KEY_D) == GLFW_PRESS;
                    if (dDown && !sWasDDown && ctrlHeld && sHoverPlacedIndex >= 0) {
                        pushVMeshUndo();
                        int del = sHoverPlacedIndex;
                        sPlacedDots.erase(sPlacedDots.begin() + del);
                        // Maintain selected-dot index across the shift.
                        if (sSelectedDotIndex == del) {
                            hideVectorSelectedTools();
                        } else if (sSelectedDotIndex > del) {
                            sSelectedDotIndex--;
                            showVectorSelectedTools();
                        }
                        sHoverPlacedIndex = -1;
                    }
                    sWasDDown = dDown;
                    Ray ray = screenToRay(mx, my, ctx.width, ctx.height,
                                          ctx.scene->view, ctx.scene->projection);

                    // Find closest line hit
                    float bestScreenDist = 999.0f;
                    glm::vec3 bestPoint;
                    bool anyHit = false;

                    // Check wireframe edges
                    for (int e = 0; e < 12; e++) {
                        LineHit hit = rayToLine(ray,
                            cubeVerts[cubeEdges[e][0]], cubeVerts[cubeEdges[e][1]],
                            mx, my, ctx.width, ctx.height,
                            ctx.scene->view, ctx.scene->projection, 10.0f);
                        if (hit.hit && hit.screenDistance < bestScreenDist) {
                            bestScreenDist = hit.screenDistance;
                            bestPoint = hit.point;
                            anyHit = true;
                        }
                    }

                    // Check placed lines
                    for (const auto& l : sPlacedLines) {
                        if (l.dotA < (int)sPlacedDots.size() && l.dotB < (int)sPlacedDots.size()) {
                            LineHit hit = rayToLine(ray,
                                sPlacedDots[l.dotA], sPlacedDots[l.dotB],
                                mx, my, ctx.width, ctx.height,
                                ctx.scene->view, ctx.scene->projection, 10.0f);
                            if (hit.hit && hit.screenDistance < bestScreenDist) {
                                bestScreenDist = hit.screenDistance;
                                bestPoint = hit.point;
                                anyHit = true;
                            }
                        }
                    }

                    if (anyHit) {
                        int snapCount = getSnapCount();
                        std::vector<glm::vec3> snaps = buildSnapPoints(snapCount);
                        int nearest = findNearestSnap(snaps, bestPoint);
                        if (nearest >= 0) {
                            sHoverPoint = snaps[nearest];
                            sHoverValid = true;
                        }
                    }

                    // Left-click handling:
                    //   - on a placed dot      -> toggle selection (live-edit panel)
                    //   - on a snapped edge    -> place a new dot
                    //   - on empty space       -> deselect
                    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    if (leftDown && !sWasLeftDown) {
                        if (sHoverPlacedIndex >= 0) {
                            sCoordEditUndoPushed = false;
                            if (sSelectedDotIndex == sHoverPlacedIndex) {
                                hideVectorSelectedTools();
                            } else {
                                sSelectedDotIndex = sHoverPlacedIndex;
                                showVectorSelectedTools();
                            }
                        } else if (sHoverValid) {
                            bool exists = false;
                            for (const auto& d : sPlacedDots)
                                if (glm::length(d - sHoverPoint) < 0.001f) { exists = true; break; }
                            if (!exists) {
                                pushVMeshUndo();
                                sPlacedDots.push_back(sHoverPoint);
                            }
                        } else {
                            if (sSelectedDotIndex >= 0) hideVectorSelectedTools();
                        }
                    }
                    sWasLeftDown = leftDown;
                }

                // Line mode: select placed dots and Ctrl+A to connect
                if (mx < ctx.width * 0.8f && sDrawMode == 1) {
                    sHoverPlacedIndex = findHoveredDot(mx, my);

                    // Detect hovered placed line
                    sHoverLineIndex = -1;
                    if (sHoverPlacedIndex < 0) {
                        Ray ray = screenToRay(mx, my, ctx.width, ctx.height,
                                              ctx.scene->view, ctx.scene->projection);
                        float bestDist = 999.0f;
                        for (int i = 0; i < (int)sPlacedLines.size(); i++) {
                            const auto& l = sPlacedLines[i];
                            if (l.dotA < (int)sPlacedDots.size() && l.dotB < (int)sPlacedDots.size()) {
                                LineHit hit = rayToLine(ray,
                                    sPlacedDots[l.dotA], sPlacedDots[l.dotB],
                                    mx, my, ctx.width, ctx.height,
                                    ctx.scene->view, ctx.scene->projection, 8.0f);
                                if (hit.hit && hit.screenDistance < bestDist) {
                                    bestDist = hit.screenDistance;
                                    sHoverLineIndex = i;
                                }
                            }
                        }
                    }

                    // Ctrl+D to delete hovered line
                    bool ctrlHeld2 = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    bool dDown2 = glfwGetKey(ctx.window, GLFW_KEY_D) == GLFW_PRESS;
                    if (dDown2 && !sWasDDown && ctrlHeld2 && sHoverLineIndex >= 0) {
                        pushVMeshUndo();
                        sPlacedLines.erase(sPlacedLines.begin() + sHoverLineIndex);
                        sHoverLineIndex = -1;
                    }
                    sWasDDown = dDown2;

                    // Left-click to select dots (max 2, third replaces first)
                    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    if (leftDown && !sWasLeftDown && sHoverPlacedIndex >= 0) {
                        // Don't add duplicate selection
                        bool alreadySelected = false;
                        for (int s : sLineSelectedDots)
                            if (s == sHoverPlacedIndex) { alreadySelected = true; break; }

                        if (!alreadySelected) {
                            if ((int)sLineSelectedDots.size() >= 2)
                                sLineSelectedDots.erase(sLineSelectedDots.begin());
                            sLineSelectedDots.push_back(sHoverPlacedIndex);
                        }
                    }
                    sWasLeftDown = leftDown;

                    // Ctrl+A to add line between 2 selected dots
                    bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    bool aDown = glfwGetKey(ctx.window, GLFW_KEY_A) == GLFW_PRESS;
                    if (aDown && !sWasADown && ctrlHeld && (int)sLineSelectedDots.size() == 2) {
                        int a = sLineSelectedDots[0];
                        int b = sLineSelectedDots[1];

                        // Check if line already exists
                        bool exists = false;
                        for (const auto& l : sPlacedLines)
                            if ((l.dotA == a && l.dotB == b) || (l.dotA == b && l.dotB == a))
                                { exists = true; break; }

                        if (!exists) {
                            pushVMeshUndo();
                            sPlacedLines.push_back({a, b});
                            sLineSelectedDots.clear();
                        }
                    }
                    sWasADown = aDown;
                }

                // Plane mode: select 3 dots, Ctrl+A to create triangle
                if (mx < ctx.width * 0.8f && sDrawMode == 2) {
                    sHoverPlacedIndex = findHoveredDot(mx, my);

                    // Detect hovered triangle
                    sHoverTriIndex = -1;
                    if (sHoverPlacedIndex < 0) {
                        Ray ray = screenToRay(mx, my, ctx.width, ctx.height,
                                              ctx.scene->view, ctx.scene->projection);
                        float bestDist = 999999.0f;
                        for (int i = 0; i < (int)sPlacedTriangles.size(); i++) {
                            const auto& tr = sPlacedTriangles[i];
                            if (tr.dotA < (int)sPlacedDots.size() && tr.dotB < (int)sPlacedDots.size() &&
                                tr.dotC < (int)sPlacedDots.size()) {
                                // Check both sides
                                TriangleHit hit = rayToTriangle(ray, sPlacedDots[tr.dotA], sPlacedDots[tr.dotB], sPlacedDots[tr.dotC]);
                                if (!hit.hit)
                                    hit = rayToTriangle(ray, sPlacedDots[tr.dotA], sPlacedDots[tr.dotC], sPlacedDots[tr.dotB]);
                                if (hit.hit && hit.distance < bestDist) {
                                    bestDist = hit.distance;
                                    sHoverTriIndex = i;
                                }
                            }
                        }
                    }

                    // Left-click to select triangle or dots
                    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                    if (leftDown && !sWasLeftDown) {
                        // Plain click  : replace selection with hovered tri
                        // Ctrl+click   : toggle hovered tri in/out of selection
                        // Shift+click  : add hovered tri to selection
                        // empty click  : clear selection
                        bool ctrlMod = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL)  == GLFW_PRESS ||
                                       glfwGetKey(ctx.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
                        bool shiftMod = glfwGetKey(ctx.window, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                                        glfwGetKey(ctx.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

                        if (sHoverTriIndex >= 0 && sHoverPlacedIndex < 0) {
                            size_t before = sSelectedTris.size();
                            if (ctrlMod) {
                                if (sSelectedTris.count(sHoverTriIndex))
                                    sSelectedTris.erase(sHoverTriIndex);
                                else
                                    sSelectedTris.insert(sHoverTriIndex);
                            } else if (shiftMod) {
                                sSelectedTris.insert(sHoverTriIndex);
                            } else {
                                sSelectedTris.clear();
                                sSelectedTris.insert(sHoverTriIndex);
                            }
                            if (sSelectedTris.size() != before) rebuildModeTools();
                        } else if (sHoverPlacedIndex < 0 && sHoverTriIndex < 0) {
                            if (!sSelectedTris.empty()) {
                                sSelectedTris.clear();
                                rebuildModeTools();
                            }
                        }
                    }
                    if (leftDown && !sWasLeftDown && sHoverPlacedIndex >= 0) {
                        bool alreadySelected = false;
                        for (int s : sPlaneSelectedDots)
                            if (s == sHoverPlacedIndex) { alreadySelected = true; break; }

                        if (!alreadySelected) {
                            if ((int)sPlaneSelectedDots.size() >= 3)
                                sPlaneSelectedDots.erase(sPlaneSelectedDots.begin());
                            sPlaneSelectedDots.push_back(sHoverPlacedIndex);
                        }
                    }
                    sWasLeftDown = leftDown;

                    // Ctrl+A to add triangle between 3 selected dots
                    bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    bool aDown = glfwGetKey(ctx.window, GLFW_KEY_A) == GLFW_PRESS;
                    if (aDown && !sWasADown && ctrlHeld && (int)sPlaneSelectedDots.size() == 3) {
                        int a = sPlaneSelectedDots[0];
                        int b = sPlaneSelectedDots[1];
                        int c = sPlaneSelectedDots[2];

                        // Check if triangle already exists (any vertex order)
                        bool exists = false;
                        for (const auto& t : sPlacedTriangles) {
                            int ta[3] = {t.dotA, t.dotB, t.dotC};
                            int na[3] = {a, b, c};
                            // Check all permutations
                            bool match = false;
                            for (int p = 0; p < 3 && !match; p++)
                                for (int q = 0; q < 3 && !match; q++)
                                    if (q != p)
                                        for (int r = 0; r < 3 && !match; r++)
                                            if (r != p && r != q)
                                                if (ta[0]==na[p] && ta[1]==na[q] && ta[2]==na[r])
                                                    match = true;
                            if (match) { exists = true; break; }
                        }

                        if (!exists) {
                            pushVMeshUndo();
                            sPlacedTriangles.push_back({a, b, c});
                            sPlaneSelectedDots.clear();
                        }
                    }
                    sWasADown = aDown;

                    // Ctrl+D: delete the multi-selection if any, else hovered.
                    bool ctrlHeld2 = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
                    bool dDown2 = glfwGetKey(ctx.window, GLFW_KEY_D) == GLFW_PRESS;
                    if (dDown2 && !sWasDDown && ctrlHeld2) {
                        if (!sSelectedTris.empty()) {
                            deleteSelectedTriangles();
                        } else if (sHoverTriIndex >= 0) {
                            pushVMeshUndo();
                            sPlacedTriangles.erase(sPlacedTriangles.begin() + sHoverTriIndex);
                            sHoverTriIndex = -1;
                        }
                    }
                    sWasDDown = dDown2;
                }
            }
        },
        // onUpdate
        nullptr,
        // onRender
        []() {
            if (sVMeshPaused)
                VE::setBrightness(0.5f);

            if (!sHideOutlines) {
                // Draw 12 edges
                glm::vec3 lineColor(1.0f, 1.0f, 1.0f);
                for (int i = 0; i < 12; i++)
                    drawLine(cubeVerts[cubeEdges[i][0]], cubeVerts[cubeEdges[i][1]], lineColor, 2.0f);

                // Vector mode rendering
                if (sDrawMode == 0) {
                    int snapCount = getSnapCount();
                    std::vector<glm::vec3> snaps = buildSnapPoints(snapCount);
                    for (const auto& s : snaps)
                        drawDot(s, 0.02f, {0.0f, 1.0f, 0.5f});

                    if (sHoverValid)
                        drawDot(sHoverPoint, 0.05f, {1.0f, 1.0f, 0.0f}, 0.4f);
                }
            }

            // Draw placed triangles
            ctx.shader->use();
            for (int i = 0; i < (int)sPlacedTriangles.size(); i++) {
                const auto& t = sPlacedTriangles[i];
                if (t.dotA < (int)sPlacedDots.size() && t.dotB < (int)sPlacedDots.size() &&
                    t.dotC < (int)sPlacedDots.size()) {
                    Triangle tri = {sPlacedDots[t.dotA], sPlacedDots[t.dotB], sPlacedDots[t.dotC]};

                    // Use winding order as placed; user can flip manually
                    Triangle front = tri;
                    if (t.flipped)
                        front = {front.v0, front.v2, front.v1};

                    Triangle back = {front.v0, front.v2, front.v1};

                    glm::vec3 color(0.8f);
                    if (sSelectedTris.count(i))
                        color = glm::vec3(0.5f, 0.7f, 1.0f);
                    else if (i == sHoverTriIndex)
                        color = glm::vec3(1.0f, 0.5f, 0.0f);

                    drawTriangleOverlay(*ctx.shader, front, color, 1.0f, false);
                    drawTriangleOverlay(*ctx.shader, back, color, 1.0f, false);
                }
            }

            if (!sHideOutlines) {
                // Draw placed lines (user-created)
                for (int i = 0; i < (int)sPlacedLines.size(); i++) {
                    const auto& l = sPlacedLines[i];
                    if (l.dotA < (int)sPlacedDots.size() && l.dotB < (int)sPlacedDots.size()) {
                        if (i == sHoverLineIndex)
                            drawLine(sPlacedDots[l.dotA], sPlacedDots[l.dotB], {1.0f, 0.5f, 0.0f}, 3.0f);
                        else
                            drawLine(sPlacedDots[l.dotA], sPlacedDots[l.dotB], {0.0f, 0.6f, 1.0f}, 2.0f);
                    }
                }

                // Draw placed dots
                for (int i = 0; i < (int)sPlacedDots.size(); i++) {
                    bool selected = false;
                    for (int s : sLineSelectedDots)
                        if (s == i) { selected = true; break; }
                    if (!selected)
                        for (int s : sPlaneSelectedDots)
                            if (s == i) { selected = true; break; }

                    if (sDrawMode == 0 && i == sSelectedDotIndex)
                        drawDot(sPlacedDots[i], 0.06f, {1.0f, 0.2f, 1.0f});
                    else if (selected)
                        drawDot(sPlacedDots[i], 0.05f, {0.0f, 0.5f, 1.0f});
                    else if (i == sHoverPlacedIndex)
                        drawDot(sPlacedDots[i], 0.05f, {1.0f, 0.5f, 0.0f});
                    else
                        drawDot(sPlacedDots[i], 0.04f, {1.0f, 0.0f, 0.0f});
                }
            }

            renderUI();
        }
    );

    // ---- AI tools ----------------------------------------------------------
    auto requireVMesh = []() {
        if (getActiveSceneName() != "vectorMesh")
            throw std::runtime_error("tool requires the vectorMesh scene to be active");
    };

    AI::registerTool("vmesh_add_vertices", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("vertices") || !a["vertices"].is_array())
            throw std::runtime_error("vertices must be an array of [x,y,z] triples");
        const auto& verts = a["vertices"];
        if (verts.empty()) throw std::runtime_error("vertices array is empty");
        // Validate all first so failures don't leave partial mutations.
        for (size_t i = 0; i < verts.size(); i++) {
            if (!verts[i].is_array() || verts[i].size() != 3)
                throw std::runtime_error("vertex " + std::to_string(i) + " must be [x,y,z]");
        }
        pushVMeshUndo();
        AI::Json indices = AI::Json::array();
        for (const auto& v : verts) {
            sPlacedDots.emplace_back(
                v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
            indices.push_back((int)sPlacedDots.size() - 1);
        }
        return AI::Json{{"indices", indices}};
    });

    AI::registerTool("vmesh_add_triangles", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("triangles") || !a["triangles"].is_array())
            throw std::runtime_error("triangles must be an array of {a,b,c[,flipped]} objects");
        const auto& tris = a["triangles"];
        if (tris.empty()) throw std::runtime_error("triangles array is empty");
        const int n = (int)sPlacedDots.size();
        for (size_t i = 0; i < tris.size(); i++) {
            const auto& t = tris[i];
            int ai = t.at("a").get<int>();
            int bi = t.at("b").get<int>();
            int ci = t.at("c").get<int>();
            if (ai < 0 || ai >= n || bi < 0 || bi >= n || ci < 0 || ci >= n)
                throw std::runtime_error("triangle " + std::to_string(i) + " has vertex index out of range");
            if (ai == bi || bi == ci || ai == ci)
                throw std::runtime_error("triangle " + std::to_string(i) + " vertices must be distinct");
        }
        pushVMeshUndo();
        AI::Json indices = AI::Json::array();
        for (const auto& t : tris) {
            PlacedTriangle pt;
            pt.dotA = t.at("a").get<int>();
            pt.dotB = t.at("b").get<int>();
            pt.dotC = t.at("c").get<int>();
            pt.flipped = t.value("flipped", false);
            sPlacedTriangles.push_back(pt);
            indices.push_back((int)sPlacedTriangles.size() - 1);
        }
        return AI::Json{{"indices", indices}};
    });

    AI::registerTool("vmesh_delete_triangles", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("indices") || !a["indices"].is_array())
            throw std::runtime_error("indices (array of triangle indices) required");
        const auto& arr = a["indices"];
        const int n = (int)sPlacedTriangles.size();
        std::unordered_set<int> toDelete;
        for (const auto& v : arr) {
            int i = v.get<int>();
            if (i < 0 || i >= n)
                throw std::runtime_error("triangle index " + std::to_string(i) + " out of range");
            toDelete.insert(i);
        }
        if (toDelete.empty()) return AI::Json{{"deleted", 0}};
        pushVMeshUndo();
        std::vector<PlacedTriangle> kept;
        kept.reserve(n - toDelete.size());
        for (int i = 0; i < n; i++)
            if (!toDelete.count(i)) kept.push_back(sPlacedTriangles[i]);
        sPlacedTriangles = std::move(kept);
        // Indices shifted; safest to drop the manual selection.
        sSelectedTris.clear();
        return AI::Json{{"deleted", (int)toDelete.size()}};
    });

    AI::registerTool("vmesh_delete_vertices", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("indices") || !a["indices"].is_array())
            throw std::runtime_error("indices (array of vertex indices) required");
        const auto& arr = a["indices"];
        const int n = (int)sPlacedDots.size();
        std::unordered_set<int> toDelete;
        for (const auto& v : arr) {
            int i = v.get<int>();
            if (i < 0 || i >= n)
                throw std::runtime_error("vertex index " + std::to_string(i) + " out of range");
            toDelete.insert(i);
        }
        if (toDelete.empty()) return AI::Json{{"deleted_vertices", 0}, {"deleted_triangles", 0}};
        pushVMeshUndo();

        // Build old->new index map.
        std::vector<int> remap(n, -1);
        int cur = 0;
        for (int i = 0; i < n; i++)
            if (!toDelete.count(i)) remap[i] = cur++;

        // Drop triangles that reference a deleted vertex; remap the rest.
        int trisDropped = 0;
        std::vector<PlacedTriangle> keptTris;
        for (const auto& t : sPlacedTriangles) {
            if (toDelete.count(t.dotA) || toDelete.count(t.dotB) || toDelete.count(t.dotC)) {
                trisDropped++;
                continue;
            }
            PlacedTriangle nt = t;
            nt.dotA = remap[t.dotA];
            nt.dotB = remap[t.dotB];
            nt.dotC = remap[t.dotC];
            keptTris.push_back(nt);
        }
        sPlacedTriangles = std::move(keptTris);

        // Same for lines.
        std::vector<PlacedLine> keptLines;
        for (const auto& l : sPlacedLines) {
            if (toDelete.count(l.dotA) || toDelete.count(l.dotB)) continue;
            keptLines.push_back({remap[l.dotA], remap[l.dotB]});
        }
        sPlacedLines = std::move(keptLines);

        // Remove vertices.
        std::vector<glm::vec3> keptDots;
        keptDots.reserve(n - toDelete.size());
        for (int i = 0; i < n; i++)
            if (!toDelete.count(i)) keptDots.push_back(sPlacedDots[i]);
        sPlacedDots = std::move(keptDots);

        sSelectedTris.clear();
        return AI::Json{
            {"deleted_vertices", (int)toDelete.size()},
            {"deleted_triangles", trisDropped}
        };
    });

    AI::registerTool("vmesh_subdivide_triangles", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("indices") || !a["indices"].is_array())
            throw std::runtime_error("indices (array of triangle indices) required");
        std::vector<int> indices;
        const int triCount = (int)sPlacedTriangles.size();
        for (const auto& v : a["indices"]) {
            int i = v.get<int>();
            if (i < 0 || i >= triCount)
                throw std::runtime_error("triangle index " + std::to_string(i) + " out of range");
            indices.push_back(i);
        }
        if (indices.empty()) return AI::Json{{"subdivided", 0}};
        pushVMeshUndo();
        int subdivided = subdivideTriangleIndices(std::move(indices));
        return AI::Json{{"subdivided", subdivided}};
    });

    AI::registerTool("vmesh_scale_from", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("origin") || !a["origin"].is_array() || a["origin"].size() != 3)
            throw std::runtime_error("origin must be [x,y,z]");
        glm::vec3 origin(
            a["origin"][0].get<float>(),
            a["origin"][1].get<float>(),
            a["origin"][2].get<float>()
        );
        float factor = a.at("factor").get<float>();
        if (factor == 0.0f) throw std::runtime_error("factor must not be zero");

        std::vector<int> indices;
        if (a.contains("indices") && a["indices"].is_array() && !a["indices"].empty()) {
            const int n = (int)sPlacedDots.size();
            for (const auto& v : a["indices"]) {
                int i = v.get<int>();
                if (i < 0 || i >= n)
                    throw std::runtime_error("vertex index " + std::to_string(i) + " out of range");
                indices.push_back(i);
            }
        } else {
            indices.resize(sPlacedDots.size());
            std::iota(indices.begin(), indices.end(), 0);
        }
        if (indices.empty()) return AI::Json{{"scaled", 0}};
        pushVMeshUndo();
        for (int i : indices) {
            sPlacedDots[i] = origin + (sPlacedDots[i] - origin) * factor;
        }
        return AI::Json{{"scaled", (int)indices.size()}, {"factor", factor}};
    });

    AI::registerTool("vmesh_perturb_vertices", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        float magnitude = a.value("magnitude", 0.05f);
        if (magnitude <= 0.0f) throw std::runtime_error("magnitude must be positive");
        std::vector<int> indices;
        if (a.contains("indices") && a["indices"].is_array() && !a["indices"].empty()) {
            const int n = (int)sPlacedDots.size();
            for (const auto& v : a["indices"]) {
                int i = v.get<int>();
                if (i < 0 || i >= n)
                    throw std::runtime_error("vertex index " + std::to_string(i) + " out of range");
                indices.push_back(i);
            }
        } else {
            indices.resize(sPlacedDots.size());
            std::iota(indices.begin(), indices.end(), 0);
        }
        if (indices.empty()) return AI::Json{{"perturbed", 0}};
        pushVMeshUndo();

        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
        for (int i : indices) {
            glm::vec3 d;
            do { d = glm::vec3(uni(rng), uni(rng), uni(rng)); }
            while (glm::length(d) < 1e-3f);
            d = glm::normalize(d) * magnitude;
            sPlacedDots[i] += d;
        }
        return AI::Json{{"perturbed", (int)indices.size()}};
    });

    AI::registerTool("vmesh_flip_triangle", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        int idx = a.at("index").get<int>();
        if (idx < 0 || idx >= (int)sPlacedTriangles.size())
            throw std::runtime_error("triangle index out of range");
        pushVMeshUndo();
        sPlacedTriangles[idx].flipped = !sPlacedTriangles[idx].flipped;
        return AI::Json{{"index", idx}, {"flipped", sPlacedTriangles[idx].flipped}};
    });

    AI::registerTool("vmesh_list_vertices", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        AI::Json arr = AI::Json::array();
        for (int i = 0; i < (int)sPlacedDots.size(); i++) {
            const auto& v = sPlacedDots[i];
            arr.push_back({{"index", i}, {"x", v.x}, {"y", v.y}, {"z", v.z}});
        }
        return arr;
    });

    AI::registerTool("vmesh_list_triangles", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        AI::Json arr = AI::Json::array();
        for (int i = 0; i < (int)sPlacedTriangles.size(); i++) {
            const auto& t = sPlacedTriangles[i];
            glm::vec3 A = sPlacedDots[t.dotA];
            glm::vec3 B = sPlacedDots[t.dotB];
            glm::vec3 C = sPlacedDots[t.dotC];
            // Match exportVMeshToMesh's blanket flip: the rendered normal is
            // cross(C-A, B-A) when not flipped, and its negation when flipped.
            glm::vec3 n = glm::normalize(glm::cross(C - A, B - A));
            if (t.flipped) n = -n;
            arr.push_back({
                {"index", i}, {"a", t.dotA}, {"b", t.dotB}, {"c", t.dotC},
                {"flipped", t.flipped},
                {"normal", {n.x, n.y, n.z}}
            });
        }
        return arr;
    });

    AI::registerTool("vmesh_save", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        if (sVMeshName.empty()) throw std::runtime_error("vmesh has no name; open one from the editor first");
        saveVectorMesh();
        exportVMeshToMesh();
        return AI::Json{{"saved", sVMeshName}};
    });

    AI::registerTool("vmesh_clear", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        pushVMeshUndo();
        sPlacedDots.clear();
        sPlacedLines.clear();
        sPlacedTriangles.clear();
        return AI::Json{{"cleared", true}};
    });

    AI::registerTool("vmesh_auto_fix_normals", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        if (sPlacedTriangles.empty())
            throw std::runtime_error("no triangles to fix");
        pushVMeshUndo();
        bool airtight = aiAutoFixNormals();
        return AI::Json{{"airtight", airtight}};
    });

    AI::registerTool("finish_vmesh_edit", [requireVMesh](const AI::Json&) -> AI::Json {
        requireVMesh();
        aiFinishVMeshEdit();
        return AI::Json{{"returned_to", sReturnModelName.empty() ? "menu" : "3dModeler"}};
    });

    // One-shot shape creation. Clears the slot, places the provided vertices
    // and triangles, auto-orients normals outward, saves, and returns to
    // 3dModeler. Preferred path for "create <shape>" prompts because it
    // removes the model's ability to bail mid-workflow.
    AI::registerTool("vmesh_build", [requireVMesh](const AI::Json& a) -> AI::Json {
        requireVMesh();
        if (!a.contains("vertices") || !a["vertices"].is_array())
            throw std::runtime_error("vertices (array of [x,y,z]) required");
        if (!a.contains("triangles") || !a["triangles"].is_array())
            throw std::runtime_error("triangles (array of {a,b,c,flipped?}) required");
        const auto& verts = a["vertices"];
        const auto& tris  = a["triangles"];
        if (verts.empty()) throw std::runtime_error("vertices is empty");
        if (tris.empty())  throw std::runtime_error("triangles is empty");

        // Validate before mutating.
        for (size_t i = 0; i < verts.size(); i++) {
            if (!verts[i].is_array() || verts[i].size() != 3)
                throw std::runtime_error("vertex " + std::to_string(i) + " must be [x,y,z]");
        }
        const int vn = (int)verts.size();
        for (size_t i = 0; i < tris.size(); i++) {
            int ai = tris[i].at("a").get<int>();
            int bi = tris[i].at("b").get<int>();
            int ci = tris[i].at("c").get<int>();
            if (ai < 0 || ai >= vn || bi < 0 || bi >= vn || ci < 0 || ci >= vn)
                throw std::runtime_error("triangle " + std::to_string(i) + " has out-of-range vertex index");
            if (ai == bi || bi == ci || ai == ci)
                throw std::runtime_error("triangle " + std::to_string(i) + " vertices must be distinct");
        }

        pushVMeshUndo();

        sPlacedDots.clear();
        sPlacedLines.clear();
        sPlacedTriangles.clear();

        for (const auto& v : verts) {
            sPlacedDots.emplace_back(
                v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
        }
        for (const auto& t : tris) {
            PlacedTriangle pt;
            pt.dotA = t.at("a").get<int>();
            pt.dotB = t.at("b").get<int>();
            pt.dotC = t.at("c").get<int>();
            pt.flipped = t.value("flipped", false);
            sPlacedTriangles.push_back(pt);
        }

        bool airtight = aiAutoFixNormals();
        // Save on disk but stay in vectorMesh so the user can review/iterate.
        saveVectorMesh();
        exportVMeshToMesh();
        return AI::Json{{"airtight", airtight}};
    });
}
