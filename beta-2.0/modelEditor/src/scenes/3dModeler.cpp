#include "3dModeler.h"
#include "../mechanics/interaction/Highlight.h"
#include "../mechanics/interaction/Selection.h"
#include "../mechanics/export/GltfExporter.h"
#include "../mechanics/export/TexturePacking.h"
#include "../../../VisualEngine/VisualEngine.h"
#include "../../../VisualEngine/EngineGlobals.h"
#include "../../../VisualEngine/inputManagement/Camera.h"
#include "../../../VisualEngine/uiManagement/UIManager.h"
#include "../../../VisualEngine/uiManagement/UIRenderer.h"
#include "../../../VisualEngine/uiManagement/UIPrefabs.h"
#include "../../../VisualEngine/uiManagement/TextRenderer.h"
#include "../../../VisualEngine/uiManagement/EmbeddedFont.h"
#include "../../../VisualEngine/memoryManagement/memory.h"
#include "../../../VisualEngine/inputManagement/Collision.h"
#include "../../../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include "../../../VisualEngine/inputManagement/Raycasting.h"
#include "../../../VisualEngine/renderingManagement/primitives/LineRenderer.h"
#include "../../../VisualEngine/renderingManagement/primitives/DotRenderer.h"
#include "../../../VisualEngine/renderingManagement/meshing/Overlay.h"
#include "../../../VisualEngine/renderingManagement/effects/RenderToTexture.h"
#include <cmath>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>
#include <fstream>
#include <iostream>
#include <random>
#include "../prefabs/3D_modeler/prefab_cube.h"
#include "../prefabs/EmbeddedSelectors.h"
#include "../prefabs/3D_modeler/prefab_wedge.h"
#include "../mechanics/AiHandling/AiHandling.h"
#include "../mechanics/AiHandling/AiTools.h"
#include "../../../VisualEngine/sceneManagement/SceneManager.h"

static bool sPaused = false;
static bool sWasEscDown = false;
static bool sWasSpaceDown = false;
static bool sWasCtrlZDown = false;
static bool sWasF5Down = false;
static bool sWasLeftDown = false;
static std::unique_ptr<Texture> sBlockSelectorPlus;
static std::unique_ptr<Texture> sBlockSelectorMinus;
static std::unique_ptr<Texture> sBlockSelectorTilde;
static const int SLOTS_PER_PAGE = 15;
static std::vector<std::string> sSlotMesh;       // empty = unassigned; size = sPageCount*SLOTS_PER_PAGE
static std::vector<RenderTarget> sSlotRT;
static std::vector<std::unique_ptr<Mesh>> sSlotPreviewMesh;
static int sSelectedSlot = -1;                   // global index
static int sSlotPage = 0;                        // currently visible pack
static int sPageCount = 1;                       // number of 15-slot packs
static float sGridCellW = 0, sGridCellH = 0, sGridPad = 0, sGridBtnX = 0, sGridY = 0;
static float sSideBtnW = 0, sSideBtnH = 0, sSideBtnX = 0, sSidePad = 0;
static float sPreviewAngle = 0.0f;
static double sLastPreviewTime = 0.0;
static int sEditorMode = 0; // 0 = Build, 1 = Paint
static bool sSoftEdges = true; // viewer-only MSAA toggle
static bool sColorEditOpen = false;
static glm::vec3 sPreEditColor{0.6f, 0.6f, 0.6f}; // snapshot taken when color edit panel opens, used by Cancel
static int sColorMode = 0; // 0 = RGB, 1 = Hex, 2 = Color Wheel
static std::function<void()> sRebuildColorInputs;
static std::function<void()> sRebuildActionButton;
// Paint palette is variable-size; one pack = 16 colors. sSelectedColor is a
// GLOBAL index into sColorWheel (pack*16 + slotInPack). The wheel UI shows the
// 16 slices for sPaintPage at a time. Selecting/scrolling stays within the
// current pack; switching packs is done via the dropdown above the wheel.
static int sSelectedColor = 0;             // global slot index
static int sHoveredColor = -1;             // global slot index, -1 if none
static int sPaintPage = 0;                 // currently visible paint pack
static int sPaintPageCount = 1;            // total paint packs (>= 1)
static const int COLORS_PER_PAINT_PACK = 16;
static std::vector<glm::vec3> sColorWheel = std::vector<glm::vec3>(COLORS_PER_PAINT_PACK, glm::vec3(0.6f, 0.6f, 0.6f));

// Undo snapshot: captures placements + palette state
struct UndoSnapshot {
    struct Entry {
        glm::ivec3 pos;
        glm::vec3 rotation;
        std::string meshName;
        std::vector<int16_t> triColors;
    };
    std::vector<Entry> placements;
    std::vector<glm::vec3> palette;
    int paintPageCount = 1;
};
static std::vector<UndoSnapshot> sUndoStack;
static const int kMaxUndoSteps = 50;

static UndoSnapshot captureSnapshot() {
    UndoSnapshot s;
    const auto& colliders = getAllColliders();
    for (const auto& col : colliders) {
        if (col.meshName == "_ghost") continue;
        UndoSnapshot::Entry e;
        e.pos = glm::ivec3(glm::round(col.position));
        e.rotation = col.rotation;
        e.meshName = col.meshName;
        e.triColors = col.triColors;
        s.placements.push_back(e);
    }
    s.palette = sColorWheel;
    s.paintPageCount = sPaintPageCount;
    return s;
}

static void pushUndo() {
    sUndoStack.push_back(captureSnapshot());
    if ((int)sUndoStack.size() > kMaxUndoSteps)
        sUndoStack.erase(sUndoStack.begin());
}

static void rebuildPaintPackDropdown();

static void restoreSnapshot(const UndoSnapshot& s) {
    VE::clearDraws();
    sColorWheel = s.palette;
    int newPackCount = (int)sColorWheel.size() / COLORS_PER_PAINT_PACK;
    if (newPackCount < 1) newPackCount = 1;
    sPaintPageCount = newPackCount;
    if (sPaintPage >= sPaintPageCount) sPaintPage = sPaintPageCount - 1;
    if (sSelectedColor >= (int)sColorWheel.size()) sSelectedColor = 0;
    setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
    for (const auto& e : s.placements) {
        VE::draw(e.meshName.c_str(), (float)e.pos.x, (float)e.pos.y, (float)e.pos.z,
                 e.rotation.x, e.rotation.y, e.rotation.z);
        BlockCollider* col = const_cast<BlockCollider*>(getColliderAt(e.pos.x, e.pos.y, e.pos.z));
        if (col) col->triColors = e.triColors;
    }
    rebuildPaintPackDropdown();
    ctx.needsRebuild = true;
}

static unsigned int sWheelVAO = 0, sWheelVBO = 0;

static void initColorWheel() {
    glGenVertexArrays(1, &sWheelVAO);
    glGenBuffers(1, &sWheelVBO);
    glBindVertexArray(sWheelVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sWheelVBO);
    glBufferData(GL_ARRAY_BUFFER, 16 * 3 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    // pos(2) + uv(2) = 4 floats per vertex
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

// Returns slice index 0..15 if cursor lies inside the quarter-circle wheel, else -1.
static int wheelSliceAtCursor() {
    double mx, my;
    glfwGetCursorPos(ctx.window, &mx, &my);
    float nx = (float)(mx / ctx.width) * 2.0f - 1.0f;
    float ny = 1.0f - (float)(my / ctx.height) * 2.0f;
    float aspect = (float)ctx.width / (float)ctx.height;
    float dx = (nx - 1.0f) * aspect;
    float dy = ny + 1.0f;
    if (dx > 0.0f || dy < 0.0f) return -1;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 0.6f) return -1;
    float deg = glm::degrees(atan2f(dy, dx));
    int slice = (int)((deg - 90.0f) / 5.625f);
    if (slice < 0) slice = 0;
    if (slice > 15) slice = 15;
    return slice;
}

static void drawColorWheel(Shader* uiShader) {
    float aspect = (float)ctx.width / (float)ctx.height;
    float radius = 0.6f;
    float cx = 1.0f;  // bottom-right corner
    float cy = -1.0f;

    float verts[16 * 3 * 4]; // 16 triangles * 3 verts * 4 floats
    int vi = 0;
    for (int i = 0; i < 16; i++) {
        float a1 = glm::radians(90.0f + i * 5.625f);
        float a2 = glm::radians(90.0f + (i + 1) * 5.625f);

        // center point
        verts[vi++] = cx; verts[vi++] = cy; verts[vi++] = 0; verts[vi++] = 0;
        // edge point 1
        verts[vi++] = cx + cosf(a1) * radius / aspect;
        verts[vi++] = cy + sinf(a1) * radius;
        verts[vi++] = 0; verts[vi++] = 0;
        // edge point 2
        verts[vi++] = cx + cosf(a2) * radius / aspect;
        verts[vi++] = cy + sinf(a2) * radius;
        verts[vi++] = 0; verts[vi++] = 0;
    }

    glBindBuffer(GL_ARRAY_BUFFER, sWheelVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    uiShader->use();
    glUniform1i(uiShader->loc("uUseTexture"), 0);
    glUniform2f(uiShader->loc("uPosition"), 0.0f, 0.0f);
    glUniform2f(uiShader->loc("uSize"), 1.0f, 1.0f);
    // The UI shader's rounded-corner SDF would clip the wheel away; disable it.
    glUniform1f(uiShader->loc("uCornerRadius"), 0.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(sWheelVAO);

    // The wheel shows the active paint pack's 16 slices. Local slot index is
    // 0..15; global = sPaintPage*16 + local.
    int packBase = sPaintPage * COLORS_PER_PAINT_PACK;
    int localSelected = (sSelectedColor >= packBase && sSelectedColor < packBase + COLORS_PER_PAINT_PACK)
        ? (sSelectedColor - packBase) : -1;
    int localHovered = (sHoveredColor >= packBase && sHoveredColor < packBase + COLORS_PER_PAINT_PACK)
        ? (sHoveredColor - packBase) : -1;

    // Draw each triangle slice
    for (int i = 0; i < 16; i++) {
        int gi = packBase + i;
        glm::vec3 c = (gi < (int)sColorWheel.size()) ? sColorWheel[gi] : glm::vec3(0.6f);
        float alpha = (i == localSelected) ? 1.0f : 0.8f;
        glUniform4f(uiShader->loc("uColor"), c.r, c.g, c.b, alpha);
        glDrawArrays(GL_TRIANGLES, i * 3, 3);
    }

    // Draw black outlines first
    glLineWidth(2.0f);
    for (int i = 0; i < 16; i++) {
        if (i == localSelected) continue;
        glUniform4f(uiShader->loc("uColor"), 0.0f, 0.0f, 0.0f, 1.0f);
        glDrawArrays(GL_LINE_LOOP, i * 3, 3);
    }
    // Hovered (non-selected) slice gets a thick grey outline.
    if (localHovered >= 0 && localHovered != localSelected) {
        glLineWidth(3.0f);
        glUniform4f(uiShader->loc("uColor"), 0.7f, 0.7f, 0.7f, 1.0f);
        glDrawArrays(GL_LINE_LOOP, localHovered * 3, 3);
    }
    // Draw selected slice and outline on top
    if (localSelected >= 0) {
        int gi = packBase + localSelected;
        glm::vec3 c = (gi < (int)sColorWheel.size()) ? sColorWheel[gi] : glm::vec3(0.6f);
        glUniform4f(uiShader->loc("uColor"), c.r, c.g, c.b, 1.0f);
        glDrawArrays(GL_TRIANGLES, localSelected * 3, 3);
        glLineWidth(3.0f);
        glUniform4f(uiShader->loc("uColor"), 1.0f, 1.0f, 0.0f, 1.0f);
        glDrawArrays(GL_LINE_LOOP, localSelected * 3, 3);
    }
    glLineWidth(1.0f);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

#include "SceneData.h"

static int sPendingSlotUpdate = -1;
static std::string sPendingSlotMesh;
static bool sFirstEntry = true;
static std::string sModelName;
static ModelFile sCurrentModel;


static void rebuildSelectorIcons();
static void rebuildPackDropdown();

static void clearSlotRT(int i) {
    bindRenderTarget(sSlotRT[i]);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    unbindRenderTarget(ctx.width, ctx.height);
}

static void allocSlotResources(int newSize) {
    int oldSize = (int)sSlotMesh.size();
    if (newSize <= oldSize) return;
    sSlotMesh.resize(newSize, "");
    sSlotRT.resize(newSize);
    sSlotPreviewMesh.resize(newSize);
    for (int i = oldSize; i < newSize; i++) {
        sSlotRT[i] = createRenderTarget(128, 128);
        clearSlotRT(i);
    }
}

// Derive pack count from existing slot_*.mesh files so packs persist across runs
// as long as each pack has at least one filled slot. An empty trailing pack is
// not remembered.
static int discoverPageCount() {
    int maxIdx = -1;
    std::error_code ec;
    std::filesystem::path dir("assets/saves/vectorMeshes");
    if (std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec)) {
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".mesh") continue;
            std::string stem = e.path().stem().string();
            if (stem.rfind("slot_", 0) != 0) continue;
            try {
                int n = std::stoi(stem.substr(5));
                if (n > maxIdx) maxIdx = n;
            } catch (...) {}
        }
    }
    if (maxIdx < 0) return 1;
    return (maxIdx / SLOTS_PER_PAGE) + 1;
}

static void addPack() {
    allocSlotResources((sPageCount + 1) * SLOTS_PER_PAGE);
    sPageCount++;
}

// Clears all 15 slots in the given pack (deletes .mesh files + in-memory state).
// The first pack cannot be deleted. Trailing empty packs are trimmed so the
// on-disk state matches the in-memory pack count.
static void deletePack(int pageIdx) {
    if (pageIdx <= 0 || pageIdx >= sPageCount) return;
    int start = pageIdx * SLOTS_PER_PAGE;
    int end   = start + SLOTS_PER_PAGE;
    if (end > (int)sSlotMesh.size()) end = (int)sSlotMesh.size();

    for (int i = start; i < end; i++) {
        if (!sSlotMesh[i].empty() && sSlotMesh[i] != "cube" && sSlotMesh[i] != "wedge") {
            std::string path = "assets/saves/vectorMeshes/" + sSlotMesh[i] + ".mesh";
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        sSlotMesh[i] = "";
        sSlotPreviewMesh[i].reset();
        clearSlotRT(i);
    }

    int newPages = sPageCount;
    while (newPages > 1) {
        int s = (newPages - 1) * SLOTS_PER_PAGE;
        int e = s + SLOTS_PER_PAGE;
        bool allEmpty = true;
        for (int i = s; i < e && i < (int)sSlotMesh.size(); i++)
            if (!sSlotMesh[i].empty()) { allEmpty = false; break; }
        if (!allEmpty) break;
        for (int i = s; i < e && i < (int)sSlotMesh.size(); i++) {
            destroyRenderTarget(sSlotRT[i]);
            sSlotPreviewMesh[i].reset();
        }
        newPages--;
    }
    if (newPages != sPageCount) {
        sPageCount = newPages;
        int sz = sPageCount * SLOTS_PER_PAGE;
        sSlotMesh.resize(sz);
        sSlotRT.resize(sz);
        sSlotPreviewMesh.resize(sz);
    }
    if (sSlotPage >= sPageCount) sSlotPage = sPageCount - 1;
    if (sSelectedSlot >= (int)sSlotMesh.size()) sSelectedSlot = 0;
}

static void rebuildSelectorIcons() {
    // Remove old selector icons and previews for the visible page
    for (int i = 0; i < SLOTS_PER_PAGE; i++) {
        removeFromGroup("sidebar", "block_" + std::to_string(i));
        removeFromGroup("sidebar", "block_" + std::to_string(i) + "_preview");
    }

    // Only show in build mode
    if (sEditorMode != 0) return;

    int cols = 5;
    int rows = 3;
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            float cx = sGridBtnX + col * (sGridCellW + sGridPad);
            float cy = sGridY - row * (sGridCellH + sGridPad);
            int localIdx = row * cols + col;
            int globalIdx = sSlotPage * SLOTS_PER_PAGE + localIdx;
            if (globalIdx >= (int)sSlotMesh.size()) continue;
            std::string id = "block_" + std::to_string(localIdx);

            unsigned int iconTex = (globalIdx == sSelectedSlot)
                ? sBlockSelectorMinus->id : sBlockSelectorPlus->id;
            auto selectSlot = [globalIdx]() {
                sSelectedSlot = globalIdx;
                if (!sSlotMesh[globalIdx].empty()) setCurrentMesh(sSlotMesh[globalIdx]);
                rebuildSelectorIcons();
            };
            UIElement img = createImage(id, cx, cy, sGridCellW, sGridCellH, iconTex);
            img.onClick = selectSlot;
            addToGroup("sidebar", img);

            if (!sSlotMesh[globalIdx].empty()) {
                UIElement preview = createImage(id + "_preview", cx, cy, sGridCellW, sGridCellH,
                    sSlotRT[globalIdx].textureId);
                preview.onClick = selectSlot;
                addToGroup("sidebar", preview);
            }
        }
    }
}

static void rebuildPackDropdown() {
    removeFromGroup("sidebar", "pack_select");
    removeUIGroup("pack_select_dropdown");
    if (sEditorMode != 0) return;

    // Position just above the top row of the grid
    float y = sGridY + sGridCellH + 0.015f;
    glm::vec4 color = {0.25f, 0.25f, 0.3f, 0.95f};

    std::vector<std::string> options;
    options.reserve(sPageCount + 1);
    for (int p = 0; p < sPageCount; p++) {
        int lo = p * SLOTS_PER_PAGE + 1;
        int hi = (p + 1) * SLOTS_PER_PAGE;
        options.push_back("Pack " + std::to_string(p + 1) +
                          " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")");
    }
    options.push_back("+ Add Pack");

    int lo = sSlotPage * SLOTS_PER_PAGE + 1;
    int hi = (sSlotPage + 1) * SLOTS_PER_PAGE;
    std::string label = "Pack " + std::to_string(sSlotPage + 1) +
                        " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")";

    // Options cascade upward so they don't cover the grid below.
    float optStep = sSideBtnH + 0.01f;
    createDropdown("sidebar", "pack_select",
                   sSideBtnX, y, sSideBtnW, sSideBtnH,
                   color, label, options,
                   [](int idx, const std::string& /*optLabel*/) {
                       if (idx == sPageCount) {
                           addPack();
                           sSlotPage = sPageCount - 1;
                       } else {
                           sSlotPage = idx;
                       }
                       rebuildPackDropdown();
                       rebuildSelectorIcons();
                   },
                   0.0f, optStep);
}

// ── Paint pack management ──────────────────────────────────────────
// Mirrors the vectorMesh model pack pattern but per-model: each pack is 16
// palette entries and lives inside ModelFile::palette (saved with the model).
// sSelectedColor is a global slot index (pack*16 + slot), so triangles painted
// in any pack keep their color when the user flips the wheel to a different
// pack.

static void addPaintPack() {
    sColorWheel.insert(sColorWheel.end(), COLORS_PER_PAINT_PACK, glm::vec3(0.6f));
    sPaintPageCount++;
    setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
}

// Removes one pack of 16 colors and remaps any triangle whose color landed in
// the deleted pack to "unpainted". Triangles in higher packs shift down.
static void deletePaintPack(int packIdx) {
    if (packIdx < 0 || packIdx >= sPaintPageCount || sPaintPageCount <= 1) return;
    int base = packIdx * COLORS_PER_PAINT_PACK;
    sColorWheel.erase(sColorWheel.begin() + base,
                      sColorWheel.begin() + base + COLORS_PER_PAINT_PACK);
    sPaintPageCount--;

    // Remap every painted triangle in the live colliders.
    auto remap = [&](int16_t idx) -> int16_t {
        if (idx < 0) return -1;
        if (idx >= base && idx < base + COLORS_PER_PAINT_PACK) return -1;
        if (idx >= base + COLORS_PER_PAINT_PACK) return idx - COLORS_PER_PAINT_PACK;
        return idx;
    };
    for (auto& col : const_cast<std::vector<BlockCollider>&>(getAllColliders()))
        for (auto& c : col.triColors) c = remap(c);

    // Also remap any pending undo snapshots so a later undo doesn't restore
    // indices that point past the now-shorter palette.
    for (auto& snap : sUndoStack) {
        for (auto& e : snap.placements)
            for (auto& c : e.triColors) c = remap(c);
        if ((int)snap.palette.size() > base + COLORS_PER_PAINT_PACK)
            snap.palette.erase(snap.palette.begin() + base,
                               snap.palette.begin() + base + COLORS_PER_PAINT_PACK);
        if (snap.paintPageCount > 1) snap.paintPageCount--;
    }

    if (sPaintPage >= sPaintPageCount) sPaintPage = sPaintPageCount - 1;
    sSelectedColor = remap((int16_t)sSelectedColor);
    if (sSelectedColor < 0) sSelectedColor = sPaintPage * COLORS_PER_PAINT_PACK;
    setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
    ctx.needsRebuild = true;
}

static void rebuildPaintPackDropdown() {
    removeFromGroup("sidebar", "paint_pack_select");
    removeUIGroup("paint_pack_select_dropdown");
    if (sEditorMode != 1) return;

    // The model pack dropdown anchors right above the grid. In Paint mode the
    // color wheel covers that region (panel + wheel are both bottom-right), so
    // lift the paint pack dropdown clear of the wheel's top.
    float y = sGridY + sGridCellH + sSideBtnH + 0.04f;
    glm::vec4 color = {0.3f, 0.25f, 0.3f, 0.95f};

    std::vector<std::string> options;
    options.reserve(sPaintPageCount + 2);
    for (int p = 0; p < sPaintPageCount; p++) {
        int lo = p * COLORS_PER_PAINT_PACK + 1;
        int hi = (p + 1) * COLORS_PER_PAINT_PACK;
        options.push_back("Paint Pack " + std::to_string(p + 1) +
                          " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")");
    }
    options.push_back("+ Add Paint Pack");
    bool canDelete = sPaintPageCount > 1;
    if (canDelete)
        options.push_back("x Delete Pack " + std::to_string(sPaintPage + 1));

    int lo = sPaintPage * COLORS_PER_PAINT_PACK + 1;
    int hi = (sPaintPage + 1) * COLORS_PER_PAINT_PACK;
    std::string label = "Paint Pack " + std::to_string(sPaintPage + 1) +
                        " (" + std::to_string(lo) + "-" + std::to_string(hi) + ")";

    float optStep = sSideBtnH + 0.01f;
    int addIdx = sPaintPageCount;
    int delIdx = canDelete ? (sPaintPageCount + 1) : -1;
    createDropdown("sidebar", "paint_pack_select",
                   sSideBtnX, y, sSideBtnW, sSideBtnH,
                   color, label, options,
                   [addIdx, delIdx](int idx, const std::string& /*optLabel*/) {
                       if (idx == addIdx) {
                           addPaintPack();
                           sPaintPage = sPaintPageCount - 1;
                           // Move selection to the new pack so edits target it.
                           sSelectedColor = sPaintPage * COLORS_PER_PAINT_PACK;
                       } else if (idx == delIdx) {
                           deletePaintPack(sPaintPage);
                       } else {
                           sPaintPage = idx;
                           // Keep selection on the same local slot in the new pack
                           // so scroll/click feel continuous.
                           int local = sSelectedColor % COLORS_PER_PAINT_PACK;
                           if (local < 0) local = 0;
                           sSelectedColor = sPaintPage * COLORS_PER_PAINT_PACK + local;
                       }
                       rebuildPaintPackDropdown();
                   },
                   0.0f, optStep);
}

static int ensureBlockType(const std::string& meshName) {
    // Find existing type
    for (int i = 0; i < (int)sCurrentModel.blockTypes.size(); i++)
        if (sCurrentModel.blockTypes[i].name == meshName) return i;

    // Add new type from registered mesh
    const RegisteredMesh* reg = getRegisteredMesh(meshName.c_str());
    if (!reg) return 0;

    BlockTypeDef bt;
    bt.name = meshName;
    bt.vertices = reg->vertices;
    bt.vertexCount = reg->vertexCount;
    bt.floatsPerVertex = reg->floatsPerVertex;
    bt.indices = reg->indices;
    bt.indexCount = reg->indexCount;
    bt.faceColors.resize(reg->indexCount / 3);
    sCurrentModel.blockTypes.push_back(bt);
    return (int)sCurrentModel.blockTypes.size() - 1;
}

int getSelectedPaintColor() {
    return sSelectedColor;
}

void pushUndoSnapshot() {
    pushUndo();
}

static void saveCurrentModel() {
    // Save palette (variable size; one pack = 16 colors)
    sCurrentModel.palette = sColorWheel;

    // Rebuild placements from current colliders
    sCurrentModel.placements.clear();
    const auto& colliders = getAllColliders();
    for (const auto& col : colliders) {
        if (col.meshName == "_ghost") continue;
        BlockPlacement p;
        p.x = (int)roundf(col.position.x);
        p.y = (int)roundf(col.position.y);
        p.z = (int)roundf(col.position.z);
        p.typeId = ensureBlockType(col.meshName);
        p.rx = (int)col.rotation.x; p.ry = (int)col.rotation.y; p.rz = (int)col.rotation.z;
        p.triColors = col.triColors;
        sCurrentModel.placements.push_back(p);
    }
    setMemoryPath("assets/saves/3dModels");
    saveModel(sModelName, sCurrentModel);
}

static void openExportDialog() {
    addUIGroup("export_dialog");

    float dlgW = 0.5f;
    float dlgH = 0.5f;
    float dlgX = -dlgW / 2.0f;
    float dlgY = -dlgH / 2.0f;
    glm::vec4 bgColor = {0.1f, 0.1f, 0.1f, 0.98f};
    glm::vec4 inputColor = {0.2f, 0.2f, 0.2f, 0.95f};

    addToGroup("export_dialog", createPanel("export_bg",
        dlgX, dlgY, dlgW, dlgH, bgColor));

    float fieldW = dlgW - 0.04f;
    float fieldX = dlgX + 0.02f;
    float fieldH = 0.08f;
    float y = dlgY + dlgH - 0.1f;

    // Name input (default: current model name)
    UIElement nameInput = createTextInput("exp_name",
        fieldX, y, fieldW, fieldH, inputColor, "Name", 48);
    nameInput.inputText = sModelName;
    addToGroup("export_dialog", nameInput);
    y -= fieldH + 0.03f;

    // Path input (optional, defaults to assets/exports)
    UIElement pathInput = createTextInput("exp_path",
        fieldX, y, fieldW, fieldH, inputColor, "Path (optional)", 128);
    addToGroup("export_dialog", pathInput);
    y -= fieldH + 0.06f;

    // Done button
    addToGroup("export_dialog", createButton("exp_done",
        fieldX, y, fieldW, fieldH,
        {0.15f, 0.5f, 0.2f, 0.95f}, "Done",
        []() {
            std::string name = getInputText("export_dialog", "exp_name");
            std::string path = getInputText("export_dialog", "exp_path");
            if (name.empty()) return;
            if (path.empty()) path = "assets/exports";
            std::filesystem::create_directories(path);
            std::string full = path + "/" + name + ".glb";
            exportModelToGlb(full);
            removeUIGroup("export_dialog");
        }
    ));
    y -= fieldH + 0.02f;

    // Cancel button
    addToGroup("export_dialog", createButton("exp_cancel",
        fieldX, y, fieldW, fieldH,
        {0.3f, 0.15f, 0.15f, 0.95f}, "Cancel",
        []() {
            removeUIGroup("export_dialog");
        }
    ));
}

static void openPauseMenu() {
    sPaused = true;

    // Exit looking mode if active
    Camera* cam = getGlobalCamera();
    if (cam->looking) {
        cam->looking = false;
        glfwSetInputMode(ctx.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    addUIGroup("pause_menu");

    float btnW = 0.3f;
    float btnH = 0.08f;
    float btnX = -btnW / 2.0f;
    glm::vec4 btnColor = {0.15f, 0.15f, 0.15f, 0.95f};

    addToGroup("pause_menu", createButton("continue",
        btnX, 0.14f, btnW, btnH, btnColor,
        "Continue",
        []() {
            sPaused = false;
            VE::setBrightness(1.0f);
            removeUIGroup("pause_menu");
        }
    ));

    addToGroup("pause_menu", createButton("export_glb",
        btnX, 0.01f, btnW, btnH, btnColor,
        "Export As GLB",
        []() {
            openExportDialog();
        }
    ));

    addToGroup("pause_menu", createButton("exit",
        btnX, -0.12f, btnW, btnH, btnColor,
        "Save & Exit",
        []() {
            saveCurrentModel();
            sPaused = false;
            sFirstEntry = true;
            VE::setBrightness(1.0f);
            removeUIGroup("pause_menu");
            VE::setScene("menu");
        }
    ));
}

void register3dModelerScene() {
    VE::registerScene("3dModeler",
        // onEnter
        [](std::shared_ptr<void> data) {
            sPaused = false;
            getGlobalCamera()->setMode(CAMERA_FPS);
            initHighlight();
            initUIRenderer();
            initTextRendererFromMemory(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE, 48);
            VE::setBrightness(1.0f);
            VE::setCamera(6, 4, 6, 210, -25);
            VE::setGradientBackground(true);
            initOverlay();
            initColorWheel();
            setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());

            // Register cube mesh for ghost block collider
            VE::MeshDef cubeDef;
            cubeDef.vertices = cubeVertices;
            cubeDef.vertexCount = 24;
            cubeDef.indices = cubePlainIndices;
            cubeDef.indexCount = 36;
            cubeDef.texturePath = nullptr;
            VE::loadMesh("_ghost", cubeDef);
            registerMeshWithStates("cube", cubeVertices, 24, cubeIndices, 12, cubeFaceStates, nullptr, true);
            registerMeshWithStates("wedge", wedgeVertices, 18, wedgeIndices, 8, wedgeFaceStates, nullptr, true);

            // Right sidebar
            float panelX = 0.6f;
            float panelW = 0.4f;
            float panelPad = 0.02f;
            float btnW = panelW - panelPad * 2;
            float btnH = 0.06f;
            float btnX = panelX + panelPad;
            float y = 0.82f;

            addUIGroup("sidebar");
            addToGroup("sidebar", createPanel("sidebar_bg",
                panelX, -1.0f, panelW, 2.0f,
                {0.12f, 0.12f, 0.12f, 0.85f}
            ));

            // Left-side AI panel: response display (top), prompt input, Send button.
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

                // Response display: dark frame with 8 pre-allocated text rows
                // on top that we fill in by word-wrapping the current response.
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

                addToGroup("ai_panel", createButton("ai_send_ctx",
                    aiBtnX, 0.26f, aiBtnW, aiBtnH,
                    {0.35f, 0.30f, 0.55f, 0.95f}, "Send + Info",
                    []() {
                        std::string text = getInputText("ai_panel", "ai_prompt");
                        if (text.empty()) return;
                        AI::submitPromptWithContext(text);
                        if (auto* in = getUIElement("ai_panel", "ai_prompt"))
                            in->inputText.clear();
                    }
                ));
            }

            // Reset slots only on first entry
            if (sFirstEntry) {
                sSlotPage = 0;
                sPageCount = discoverPageCount();
                sSlotMesh.assign(sPageCount * SLOTS_PER_PAGE, "");
                sSlotMesh[0] = "cube";
                sSlotMesh[1] = "wedge";

                // Restore custom slots from existing .mesh files
                for (int i = 0; i < (int)sSlotMesh.size(); i++) {
                    if (!sSlotMesh[i].empty()) continue;
                    std::string slotName = "slot_" + std::to_string(i);
                    std::string meshPath = "assets/saves/vectorMeshes/" + slotName + ".mesh";
                    std::ifstream check(meshPath);
                    if (check.good()) {
                        check.close();
                        sSlotMesh[i] = slotName;
                    }
                }

                sSelectedSlot = 0;
                setCurrentMesh("cube");
                sFirstEntry = false;
            }

            // Load block selector textures
            sBlockSelectorPlus = std::make_unique<Texture>(EMBEDDED_SELECTOR_PLUS, EMBEDDED_SELECTOR_PLUS_SIZE);
            sBlockSelectorMinus = std::make_unique<Texture>(EMBEDDED_SELECTOR_MINUS, EMBEDDED_SELECTOR_MINUS_SIZE);
            sBlockSelectorTilde = std::make_unique<Texture>(EMBEDDED_SELECTOR_TILDE, EMBEDDED_SELECTOR_TILDE_SIZE);
            sPreviewAngle = 0.0f;
            sLastPreviewTime = glfwGetTime();

            int totalSlots = (int)sSlotMesh.size();

            // Reload all custom slot meshes from files
            for (int i = 0; i < totalSlots; i++) {
                if (sSlotMesh[i].empty() || sSlotMesh[i] == "cube" || sSlotMesh[i] == "wedge")
                    continue;
                std::string meshPath = "assets/saves/vectorMeshes/" + sSlotMesh[i] + ".mesh";
                std::ifstream checkFile(meshPath);
                if (checkFile.good()) {
                    checkFile.close();
                    VE::loadMesh(sSlotMesh[i].c_str(), meshPath.c_str());
                }
            }

            // Create render targets and preview meshes for slots
            sSlotRT.assign(totalSlots, RenderTarget{});
            sSlotPreviewMesh.clear();
            sSlotPreviewMesh.resize(totalSlots);
            for (int i = 0; i < totalSlots; i++) {
                sSlotRT[i] = createRenderTarget(128, 128);
                clearSlotRT(i);
                if (!sSlotMesh[i].empty()) {
                    const RegisteredMesh* reg = getRegisteredMesh(sSlotMesh[i].c_str());
                    if (reg) {
                        if (reg->floatsPerVertex == 8)
                            sSlotPreviewMesh[i] = std::make_unique<Mesh>(
                                const_cast<float*>(reg->vertices.data()), reg->vertexCount,
                                const_cast<unsigned int*>(reg->indices.data()), reg->indexCount, true);
                        else
                            sSlotPreviewMesh[i] = std::make_unique<Mesh>(
                                const_cast<float*>(reg->vertices.data()), reg->vertexCount,
                                const_cast<unsigned int*>(reg->indices.data()), reg->indexCount);
                    }
                }
            }

            // Store grid layout constants (anchored to bottom of panel)
            {
                int cols = 5;
                int rows = 3;
                sGridPad = 0.005f;
                float aspect = (float)ctx.width / (float)ctx.height;
                sGridCellW = (btnW - sGridPad * (cols - 1)) / cols;
                sGridCellH = sGridCellW * aspect;
                sGridBtnX = btnX;
                // Position grid at bottom of panel
                float gridTotalH = rows * (sGridCellH + sGridPad) - sGridPad;
                sGridY = -1.0f + panelPad + gridTotalH;
            }

            // Store sidebar layout for later use
            sSideBtnW = btnW;
            sSideBtnH = btnH;
            sSideBtnX = btnX;
            sSidePad = panelPad;

            rebuildSelectorIcons();
            rebuildPackDropdown();
            rebuildPaintPackDropdown();

            // Action button (changes based on editor mode)
            {
                float actionBtnY = y;
                auto rebuildActionButton = [btnX, btnW, btnH, actionBtnY]() {
                    removeFromGroup("sidebar", "action_btn");
                    // Close color edit panel if open
                    if (sColorEditOpen) {
                        removeUIGroup("color_edit");
                        removeUIGroup("color_mode_dropdown");
                        sColorEditOpen = false;
                    }
                    if (sEditorMode == 0) {
                        addToGroup("sidebar", createButton("action_btn",
                            btnX, actionBtnY, btnW, btnH,
                            {0.25f, 0.25f, 0.3f, 0.95f}, "Edit Object",
                            []() {
                                // Don't allow editing prefab meshes
                                const std::string& cur = sSlotMesh[sSelectedSlot];
                                if (!cur.empty()) {
                                    const RegisteredMesh* reg = getRegisteredMesh(cur.c_str());
                                    if (reg && reg->isPrefab) return;
                                }

                                saveCurrentModel();
                                std::string meshName = cur.empty()
                                    ? "slot_" + std::to_string(sSelectedSlot)
                                    : cur;
                                sPendingSlotUpdate = sSelectedSlot;
                                sPendingSlotMesh = meshName;
                                VE::setScene("vectorMesh", std::make_shared<VectorMeshEditData>(VectorMeshEditData{meshName, sSelectedSlot, sModelName}));
                            }
                        ));
                    } else {
                        glm::vec4 actionBtnColor = sColorEditOpen
                            ? glm::vec4{0.4f, 0.25f, 0.25f, 0.95f}
                            : glm::vec4{0.3f, 0.25f, 0.25f, 0.95f};
                        addToGroup("sidebar", createButton("action_btn",
                            btnX, actionBtnY, btnW, btnH,
                            actionBtnColor, sColorEditOpen ? "Cancel" : "Edit Color",
                            [btnX, btnW, btnH, actionBtnY]() {
                                sColorEditOpen = !sColorEditOpen;
                                auto closeColorEdit = []() {
                                    sColorEditOpen = false;
                                    removeUIGroup("color_edit");
                                    removeUIGroup("color_mode_dropdown");
                                    UIElement* editorMode = getUIElement("sidebar", "editor_mode");
                                    if (editorMode) editorMode->visible = true;
                                    UIElement* actionBtn = getUIElement("sidebar", "action_btn");
                                    if (actionBtn) {
                                        actionBtn->label = "Edit Color";
                                        actionBtn->color = {0.3f, 0.25f, 0.25f, 0.95f};
                                    }
                                };

                                if (sColorEditOpen) {
                                    // Snapshot current color so Cancel can revert
                                    if (sSelectedColor >= 0 && sSelectedColor < (int)sColorWheel.size())
                                        sPreEditColor = sColorWheel[sSelectedColor];

                                    UIElement* actionBtn = getUIElement("sidebar", "action_btn");
                                    if (actionBtn) {
                                        actionBtn->label = "Cancel";
                                        actionBtn->color = {0.4f, 0.25f, 0.25f, 0.95f};
                                    }

                                    // Hide editor mode dropdown
                                    UIElement* editorMode = getUIElement("sidebar", "editor_mode");
                                    if (editorMode) editorMode->visible = false;

                                    float panelPad = 0.02f;
                                    float inputsY = actionBtnY - btnH - panelPad;

                                    addUIGroup("color_edit");

                                    // Color mode dropdown
                                    std::string modeLabel = sColorMode == 0 ? "RGB" : sColorMode == 1 ? "Hex" : "Color Wheel";
                                    createDropdown("color_edit", "color_mode",
                                        btnX, inputsY, btnW, btnH,
                                        {0.18f, 0.18f, 0.18f, 0.95f},
                                        modeLabel,
                                        {"RGB", "Hex"},
                                        [btnX, btnW, btnH, actionBtnY](int index, const std::string& option) {
                                            sColorMode = index;
                                            if (sRebuildColorInputs) sRebuildColorInputs();
                                        },
                                        0.0f, -(btnH + panelPad)
                                    );
                                    inputsY -= btnH + panelPad;

                                    // Store inputsY for rebuild
                                    float fieldsY = inputsY;

                                    auto buildInputs = [btnX, btnW, btnH, fieldsY, panelPad, closeColorEdit]() {
                                        // Remove old inputs
                                        removeFromGroup("color_edit", "color_r");
                                        removeFromGroup("color_edit", "color_g");
                                        removeFromGroup("color_edit", "color_b");
                                        removeFromGroup("color_edit", "color_br");
                                        removeFromGroup("color_edit", "color_hex");
                                        removeFromGroup("color_edit", "color_hex_br");
                                        removeFromGroup("color_edit", "color_done");

                                        float y = fieldsY;
                                        glm::vec4 inputColor = {0.2f, 0.2f, 0.2f, 0.95f};

                                        glm::vec3 cur = (sSelectedColor >= 0 && sSelectedColor < (int)sColorWheel.size())
                                            ? sColorWheel[sSelectedColor] : glm::vec3(0.6f);
                                        bool isDefault = (cur == glm::vec3(0.6f));
                                        float maxC = std::max({cur.r, cur.g, cur.b, 0.001f});

                                        if (sColorMode == 0) {
                                            // RGB mode
                                            float inputPad = 0.005f;
                                            float inputW = (btnW - inputPad * 3) / 4.0f;

                                            int ri = isDefault ? 0 : (int)std::round(cur.r / maxC * 255.0f);
                                            int gi = isDefault ? 0 : (int)std::round(cur.g / maxC * 255.0f);
                                            int bi = isDefault ? 0 : (int)std::round(cur.b / maxC * 255.0f);
                                            int bri = isDefault ? 0 : (int)std::round(maxC * 255.0f);

                                            auto mkInput = [&](const std::string& id, float x, const std::string& ph, int val) {
                                                UIElement e = createTextInput(id, x, y, inputW, btnH, inputColor, ph, 3);
                                                if (!isDefault) e.inputText = std::to_string(val);
                                                return e;
                                            };
                                            addToGroup("color_edit", mkInput("color_r", btnX, "R", ri));
                                            addToGroup("color_edit", mkInput("color_g", btnX + inputW + inputPad, "G", gi));
                                            addToGroup("color_edit", mkInput("color_b", btnX + (inputW + inputPad) * 2, "B", bi));
                                            addToGroup("color_edit", mkInput("color_br", btnX + (inputW + inputPad) * 3, "Bri", bri));
                                        } else if (sColorMode == 1) {
                                            // Hex mode
                                            float inputPad = 0.005f;
                                            float hexW = btnW * 0.7f;
                                            float briW = btnW - hexW - inputPad;

                                            // Convert to hex string
                                            int ri = isDefault ? 0 : (int)std::round(cur.r / maxC * 255.0f);
                                            int gi = isDefault ? 0 : (int)std::round(cur.g / maxC * 255.0f);
                                            int bi = isDefault ? 0 : (int)std::round(cur.b / maxC * 255.0f);
                                            int bri = isDefault ? 0 : (int)std::round(maxC * 255.0f);

                                            char hexBuf[8];
                                            snprintf(hexBuf, sizeof(hexBuf), "%02X%02X%02X", ri, gi, bi);

                                            UIElement hexInput = createTextInput("color_hex",
                                                btnX, y, hexW, btnH, inputColor, "Hex", 6);
                                            hexInput.hexOnly = true;
                                            if (!isDefault) hexInput.inputText = hexBuf;
                                            addToGroup("color_edit", hexInput);

                                            UIElement briInput = createTextInput("color_hex_br",
                                                btnX + hexW + inputPad, y, briW, btnH, inputColor, "Bri", 3);
                                            if (!isDefault) briInput.inputText = std::to_string(bri);
                                            addToGroup("color_edit", briInput);
                                        }
                                        y -= btnH + panelPad;

                                        // Done button
                                        addToGroup("color_edit", createButton("color_done",
                                            btnX, y, btnW, btnH,
                                            {0.15f, 0.5f, 0.2f, 0.95f}, "Done",
                                            [closeColorEdit]() {
                                                float r = 0, g = 0, b = 0, bri = 1.0f;

                                                auto safeParseByte = [](const std::string& s, float fallback) -> float {
                                                    if (s.empty()) return fallback;
                                                    try {
                                                        return std::clamp(std::stof(s) / 255.0f, 0.0f, 1.0f);
                                                    } catch (const std::exception&) {
                                                        return fallback;
                                                    }
                                                };
                                                if (sColorMode == 0) {
                                                    std::string rS = getInputText("color_edit", "color_r");
                                                    std::string gS = getInputText("color_edit", "color_g");
                                                    std::string bS = getInputText("color_edit", "color_b");
                                                    std::string brS = getInputText("color_edit", "color_br");
                                                    r = safeParseByte(rS, 0.0f);
                                                    g = safeParseByte(gS, 0.0f);
                                                    b = safeParseByte(bS, 0.0f);
                                                    bri = safeParseByte(brS, 1.0f);
                                                } else if (sColorMode == 1) {
                                                    std::string hex = getInputText("color_edit", "color_hex");
                                                    std::string brS = getInputText("color_edit", "color_hex_br");
                                                    if (hex.size() == 6) {
                                                        try {
                                                            size_t consumed = 0;
                                                            unsigned int val = std::stoul(hex, &consumed, 16);
                                                            if (consumed == 6) {
                                                                r = ((val >> 16) & 0xFF) / 255.0f;
                                                                g = ((val >> 8) & 0xFF) / 255.0f;
                                                                b = (val & 0xFF) / 255.0f;
                                                            }
                                                        } catch (const std::exception&) {
                                                            // Unparseable hex — leave r/g/b at 0.
                                                        }
                                                    }
                                                    bri = safeParseByte(brS, 1.0f);
                                                }

                                                if (sSelectedColor >= 0 && sSelectedColor < (int)sColorWheel.size())
                                                    sColorWheel[sSelectedColor] = glm::vec3(r, g, b) * bri;

                                                setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
                                                ctx.needsRebuild = true;
                                                closeColorEdit();
                                            }
                                        ));
                                    };

                                    sRebuildColorInputs = buildInputs;
                                    buildInputs();
                                } else {
                                    // Cancel: revert color to snapshot taken when panel opened
                                    if (sSelectedColor >= 0 && sSelectedColor < (int)sColorWheel.size()) {
                                        sColorWheel[sSelectedColor] = sPreEditColor;
                                        setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
                                        ctx.needsRebuild = true;
                                    }
                                    closeColorEdit();
                                }
                            }
                        ));
                    }
                };
                rebuildActionButton();
                sRebuildActionButton = rebuildActionButton;
            }
            y -= btnH + panelPad;

            // Soft Edges toggle (MSAA). Viewer-only — not saved with the model.
            if (sSoftEdges) glEnable(GL_MULTISAMPLE);
            else            glDisable(GL_MULTISAMPLE);
            {
                glm::vec4 onColor  = {0.25f, 0.35f, 0.25f, 0.95f};
                glm::vec4 offColor = {0.18f, 0.18f, 0.18f, 0.95f};
                std::string label = sSoftEdges ? "Soft Edges: On" : "Soft Edges: Off";
                auto btn = createButton("soft_edges_toggle",
                    btnX, y, btnW, btnH,
                    sSoftEdges ? onColor : offColor,
                    label,
                    [onColor, offColor]() {
                        sSoftEdges = !sSoftEdges;
                        if (sSoftEdges) glEnable(GL_MULTISAMPLE);
                        else            glDisable(GL_MULTISAMPLE);
                        UIElement* el = getUIElement("sidebar", "soft_edges_toggle");
                        if (el) {
                            el->label = sSoftEdges ? "Soft Edges: On" : "Soft Edges: Off";
                            el->color = sSoftEdges ? onColor : offColor;
                        }
                    });
                addToGroup("sidebar", btn);
            }
            y -= btnH + panelPad;

            // Editor mode dropdown
            createDropdown("sidebar", "editor_mode",
                btnX, y, btnW, btnH,
                {0.18f, 0.18f, 0.18f, 0.95f},
                "Build",
                {"Build", "Paint"},
                [](int index, const std::string& option) {
                    sEditorMode = index;
                    clearSelection();
                    rebuildSelectorIcons();
                    rebuildPackDropdown();
                    rebuildPaintPackDropdown();
                    if (sRebuildActionButton) sRebuildActionButton();
                    if (sEditorMode == 1 && sSelectedColor < 0)
                        sSelectedColor = 0;
                },
                0.0f, -(btnH + panelPad)
            );
            y -= btnH + panelPad;

            // Load model if name was passed
            if (data) {
                auto name = std::static_pointer_cast<std::string>(data);
                sModelName = *name;
                setMemoryPath("assets/saves/3dModels");
                sUndoStack.clear();
                if (loadModel(sModelName, sCurrentModel)) {
                    // Restore palette (variable size; one pack = 16 colors).
                    // Round up to a multiple of 16 in case a future writer left
                    // a partial pack on disk.
                    sColorWheel = sCurrentModel.palette;
                    if (sColorWheel.empty())
                        sColorWheel.assign(COLORS_PER_PAINT_PACK, glm::vec3(0.6f));
                    int rem = (int)sColorWheel.size() % COLORS_PER_PAINT_PACK;
                    if (rem != 0)
                        sColorWheel.resize(sColorWheel.size() + (COLORS_PER_PAINT_PACK - rem), glm::vec3(0.6f));
                    sPaintPageCount = (int)sColorWheel.size() / COLORS_PER_PAINT_PACK;
                    if (sPaintPage >= sPaintPageCount) sPaintPage = sPaintPageCount - 1;
                    if (sSelectedColor >= (int)sColorWheel.size()) sSelectedColor = 0;
                    setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
                    rebuildPaintPackDropdown();

                    for (int i = 0; i < (int)sCurrentModel.blockTypes.size(); i++) {
                        const BlockTypeDef& bt = sCurrentModel.blockTypes[i];
                        // Skip prefabs — already registered with correct face states
                        const RegisteredMesh* existing = getRegisteredMesh(bt.name.c_str());
                        if (existing && existing->isPrefab) continue;

                        if (bt.floatsPerVertex == 8) {
                            // VN mesh: load directly from .mesh file
                            std::string meshPath = "assets/saves/vectorMeshes/" + bt.name + ".mesh";
                            VE::loadMesh(bt.name.c_str(), meshPath.c_str());
                        } else {
                            VE::MeshDef def;
                            def.vertices = const_cast<float*>(bt.vertices.data());
                            def.vertexCount = bt.vertexCount;
                            def.indices = const_cast<unsigned int*>(bt.indices.data());
                            def.indexCount = bt.indexCount;
                            def.texturePath = bt.texturePath.empty() ? nullptr : bt.texturePath.c_str();
                            VE::loadMesh(bt.name.c_str(), def);
                        }
                    }
                    for (const auto& p : sCurrentModel.placements) {
                        if (p.typeId < (int)sCurrentModel.blockTypes.size()) {
                            VE::draw(sCurrentModel.blockTypes[p.typeId].name.c_str(),
                                     (float)p.x, (float)p.y, (float)p.z,
                                     (float)p.rx, (float)p.ry, (float)p.rz);
                            // Restore paint colors
                            if (!p.triColors.empty()) {
                                BlockCollider* col = const_cast<BlockCollider*>(
                                    getColliderAt(p.x, p.y, p.z));
                                if (col) col->triColors = p.triColors;
                            }
                        }
                    }
                }
            }

            // Apply pending slot update from vector mesh editor
            if (sPendingSlotUpdate >= 0) {
                std::string meshPath = "assets/saves/vectorMeshes/" + sPendingSlotMesh + ".mesh";
                std::ifstream check(meshPath);
                if (check.good()) {
                    check.close();
                    VE::loadMesh(sPendingSlotMesh.c_str(), meshPath.c_str());
                    sSlotMesh[sPendingSlotUpdate] = sPendingSlotMesh;
                    sSelectedSlot = sPendingSlotUpdate;
                    setCurrentMesh(sPendingSlotMesh);

                    // Create preview mesh for the slot
                    const RegisteredMesh* reg = getRegisteredMesh(sPendingSlotMesh.c_str());
                    if (reg) {
                        if (reg->floatsPerVertex == 8)
                            sSlotPreviewMesh[sPendingSlotUpdate] = std::make_unique<Mesh>(
                                const_cast<float*>(reg->vertices.data()), reg->vertexCount,
                                const_cast<unsigned int*>(reg->indices.data()), reg->indexCount, true);
                        else
                            sSlotPreviewMesh[sPendingSlotUpdate] = std::make_unique<Mesh>(
                                const_cast<float*>(reg->vertices.data()), reg->vertexCount,
                                const_cast<unsigned int*>(reg->indices.data()), reg->indexCount);
                    }
                    rebuildSelectorIcons();
                }
                sPendingSlotUpdate = -1;
                sPendingSlotMesh = "";
            }

            // Add ghost collider at origin if no real block there
            if (!VE::hasBlockAt(0, 0, 0)) {
                const RegisteredMesh* reg = getRegisteredMesh("_ghost");
                if (reg)
                    addCollider("_ghost", reg->vertices.data(), reg->vertexCount,
                                reg->indices.data(), reg->indexCount, reg->rectangular, 0, 0, 0);
            }
        },
        // onExit
        []() {
            // Auto-save on exit (window close or scene switch)
            if (!sModelName.empty())
                saveCurrentModel();
            cleanupHighlight();
            cleanupOverlay();
            sBlockSelectorPlus.reset();
            sBlockSelectorMinus.reset();
            sBlockSelectorTilde.reset();
            for (int i = 0; i < (int)sSlotRT.size(); i++) {
                destroyRenderTarget(sSlotRT[i]);
                sSlotPreviewMesh[i].reset();
            }
            cleanupTextRenderer();
            cleanupUIRenderer();
            clearUI();
            VE::clearDraws();
            VE::setGradientBackground(false);
        },
        // onInput
        [](float dt) {
            AI::update(dt);

            // Grow the AI prompt vertically to fit its current text; reposition
            // Send below it. Top edge stays anchored at 0.48.
            if (auto* input = getUIElement("ai_panel", "ai_prompt")) {
                const float kBaseH = 0.06f;    // one-line height (matches initial)
                const float kLineGrow = 0.05f; // extra height per additional line
                const float kTopEdge = 0.48f;
                int lines = inputWrappedLineCount(*input);
                float newH = kBaseH + kLineGrow * (float)std::max(0, lines - 1);
                input->size.y = newH;
                input->position.y = kTopEdge - newH;
                if (auto* send = getUIElement("ai_panel", "ai_send")) {
                    send->position.y = input->position.y - 0.02f - 0.06f;
                    if (auto* sendCtx = getUIElement("ai_panel", "ai_send_ctx")) {
                        sendCtx->position.y = send->position.y - 0.02f - 0.06f;
                    }
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

            // Build mode = AABB selection, Paint mode = per-triangle
            setForceRectangularRaycast(sEditorMode == 0);

            // Block camera when paused
            if (sPaused) {
                Camera* cam = getGlobalCamera();
                if (cam->looking) {
                    cam->looking = false;
                    glfwSetInputMode(ctx.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                }
            }

            bool escDown = glfwGetKey(ctx.window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escDown && !sWasEscDown) {
                if (sPaused) {
                    sPaused = false;
                    VE::setBrightness(1.0f);
                    removeUIGroup("pause_menu");
                } else {
                    openPauseMenu();
                }
            }
            sWasEscDown = escDown;

            if (sPaused) {
                processUIInput();
                return;
            }

            // Scroll: build mode = cycle slots, paint mode = cycle colors
            // (within the current paint pack only).
            if (ctx.scrollDelta != 0.0f && sEditorMode == 1) {
                int dir = (ctx.scrollDelta > 0.0f) ? 1 : -1;
                int packBase = sPaintPage * COLORS_PER_PAINT_PACK;
                int local = sSelectedColor - packBase;
                if (local < 0 || local >= COLORS_PER_PAINT_PACK) local = 0;
                local = (local + dir + COLORS_PER_PAINT_PACK) % COLORS_PER_PAINT_PACK;
                sSelectedColor = packBase + local;
            }
            if (ctx.scrollDelta != 0.0f && sEditorMode == 0) {
                int total = (int)sSlotMesh.size();
                if (total > 0) {
                    int dir = (ctx.scrollDelta > 0.0f) ? -1 : 1;
                    int prevPage = sSlotPage;
                    sSelectedSlot = (sSelectedSlot + dir + total) % total;
                    sSlotPage = sSelectedSlot / SLOTS_PER_PAGE;
                    if (!sSlotMesh[sSelectedSlot].empty())
                        setCurrentMesh(sSlotMesh[sSelectedSlot]);
                    if (sSlotPage != prevPage) rebuildPackDropdown();
                    rebuildSelectorIcons();
                }
            }

            // Space: shortcut for Edit Object (build) / Edit Color (paint) - fires on release.
            // Ignored while in camera movement mode so space can be used for jump/up.
            bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
            Camera* spaceCam = getGlobalCamera();
            bool inMovement = spaceCam && spaceCam->looking;
            bool spaceDown = !inMovement && !isAnyInputFocused()
                             && glfwGetKey(ctx.window, GLFW_KEY_SPACE) == GLFW_PRESS;
            if (!spaceDown && sWasSpaceDown) {
                if (sEditorMode == 0) {
                    const std::string& cur = sSlotMesh[sSelectedSlot];
                    bool isPrefab = false;
                    if (!cur.empty()) {
                        const RegisteredMesh* reg = getRegisteredMesh(cur.c_str());
                        if (reg && reg->isPrefab) isPrefab = true;
                    }
                    if (!isPrefab) {
                        saveCurrentModel();
                        std::string meshName = cur.empty()
                            ? "slot_" + std::to_string(sSelectedSlot)
                            : cur;
                        sPendingSlotUpdate = sSelectedSlot;
                        sPendingSlotMesh = meshName;
                        VE::setScene("vectorMesh", std::make_shared<VectorMeshEditData>(VectorMeshEditData{meshName, sSelectedSlot, sModelName}));
                    }
                } else {
                    UIElement* actionBtn = getUIElement("sidebar", "action_btn");
                    if (actionBtn && actionBtn->onClick) actionBtn->onClick();
                }
            }
            sWasSpaceDown = spaceDown;

            // Ctrl+Z: undo
            bool zDown = glfwGetKey(ctx.window, GLFW_KEY_Z) == GLFW_PRESS;
            bool ctrlZ = ctrlHeld && zDown;
            if (ctrlZ && !sWasCtrlZDown) {
                if (!sUndoStack.empty()) {
                    UndoSnapshot s = sUndoStack.back();
                    sUndoStack.pop_back();
                    restoreSnapshot(s);
                }
            }
            sWasCtrlZDown = ctrlZ;

            // F5: test texture packing
            bool f5Down = glfwGetKey(ctx.window, GLFW_KEY_F5) == GLFW_PRESS;
            if (f5Down && !sWasF5Down) {
                // Test: 4 verts making 2 triangles, red and blue
                std::vector<glm::vec2> verts = {
                    {0.5f, 0.0f},   // a (0)
                    {1.0f, 0.0f},   // b (1)
                    {1.0f, 0.75f},  // c (2)
                    {0.0f, 0.6f},   // d (3)
                };
                std::vector<uint32_t> indices = {
                    0, 1, 2,  // a,b,c
                    0, 2, 3,  // a,c,d
                };
                std::vector<glm::vec3> colors = {
                    {1.0f, 0.0f, 0.0f}, // red (a,b,c)
                    {0.0f, 0.0f, 1.0f}, // blue (a,c,d)
                };
                auto pngBytes = packTrianglesToPNG(verts, indices, colors, 128);
                if (!pngBytes.empty()) {
                    std::filesystem::create_directories("assets/exports");
                    std::ofstream out("assets/exports/test_packing.png", std::ios::binary);
                    out.write((const char*)pngBytes.data(), pngBytes.size());
                    std::cout << "[Test] Wrote test_packing.png (" << pngBytes.size() << " bytes)" << std::endl;
                } else {
                    std::cout << "[Test] packTrianglesToPNG returned empty" << std::endl;
                }
            }
            sWasF5Down = f5Down;

            processUIInput();

            // Block selector hover (build mode): any unselected slot (empty
            // or filled) swaps its base icon to the tilde (hover) variant.
            int hoveredSlot = -1; // global idx
            if (sEditorMode == 0) {
                double bmx, bmy;
                glfwGetCursorPos(ctx.window, &bmx, &bmy);
                float nx = (float)(bmx / ctx.width) * 2.0f - 1.0f;
                float ny = 1.0f - (float)(bmy / ctx.height) * 2.0f;
                for (int row = 0; row < 3 && hoveredSlot < 0; row++) {
                    for (int col = 0; col < 5; col++) {
                        int localIdx = row * 5 + col;
                        int globalIdx = sSlotPage * SLOTS_PER_PAGE + localIdx;
                        if (globalIdx >= (int)sSlotMesh.size()) continue;
                        if (globalIdx == sSelectedSlot) continue;
                        float cx = sGridBtnX + col * (sGridCellW + sGridPad);
                        float cy = sGridY - row * (sGridCellH + sGridPad);
                        if (nx >= cx && nx <= cx + sGridCellW &&
                            ny >= cy && ny <= cy + sGridCellH) {
                            hoveredSlot = globalIdx;
                            break;
                        }
                    }
                }
            }
            for (int localIdx = 0; localIdx < SLOTS_PER_PAGE; localIdx++) {
                UIElement* el = getUIElement("sidebar", "block_" + std::to_string(localIdx));
                if (!el) continue;
                int globalIdx = sSlotPage * SLOTS_PER_PAGE + localIdx;
                if (globalIdx == sSelectedSlot)      el->textureId = sBlockSelectorMinus->id;
                else if (globalIdx == hoveredSlot)   el->textureId = sBlockSelectorTilde->id;
                else                                 el->textureId = sBlockSelectorPlus->id;
            }

            // Color wheel hover + click (paint mode). Wheel is raw GL so
            // interactions live here, not in the UI manager. Wheel slice
            // indices are LOCAL (0..15); we map to global by adding the
            // active pack base.
            sHoveredColor = -1;
            if (sEditorMode == 1) {
                double mx, my;
                glfwGetCursorPos(ctx.window, &mx, &my);
                if (!isPointOverUI(mx, my, ctx.width, ctx.height)) {
                    int local = wheelSliceAtCursor();
                    if (local >= 0)
                        sHoveredColor = sPaintPage * COLORS_PER_PAINT_PACK + local;
                }
                bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (leftDown && !sWasLeftDown && sHoveredColor >= 0) {
                    sSelectedColor = sHoveredColor;
                }
                sWasLeftDown = leftDown;
            } else {
                sWasLeftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            }
        },
        // onUpdate
        []() {
            // Re-add ghost collider at origin if block was removed
            if (!VE::hasBlockAt(0, 0, 0)) {
                const BlockCollider* col = getColliderAt(0, 0, 0);
                if (!col) {
                    const RegisteredMesh* reg = getRegisteredMesh("_ghost");
                    if (reg)
                        addCollider("_ghost", reg->vertices.data(), reg->vertexCount,
                                    reg->indices.data(), reg->indexCount, reg->rectangular, 0, 0, 0);
                }
            }
        },
        // onRender
        []() {
            // Apply brightness dim while paused
            if (sPaused)
                VE::setBrightness(0.5f);

            // Render preview meshes into slot render targets
            double now = glfwGetTime();
            float dt = (float)(now - sLastPreviewTime);
            sLastPreviewTime = now;
            sPreviewAngle += 15.0f * dt; // 15 degrees per second
            if (sPreviewAngle > 360.0f) sPreviewAngle -= 360.0f;

            for (int i = 0; i < (int)sSlotPreviewMesh.size(); i++) {
                if (!sSlotPreviewMesh[i]) continue;

                bindRenderTarget(sSlotRT[i]);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                ctx.shader->use();

                glm::mat4 previewView = glm::lookAt(
                    glm::vec3(3.0f, 2.0f, 3.0f),
                    glm::vec3(0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f)
                );
                glm::mat4 previewProj = glm::perspective(glm::radians(35.0f), 1.0f, 0.1f, 20.0f);
                glm::mat4 previewModel = glm::rotate(glm::mat4(1.0f),
                    glm::radians(sPreviewAngle), glm::vec3(0.0f, 1.0f, 0.0f));

                glUniformMatrix4fv(ctx.shader->loc("view"), 1, GL_FALSE, glm::value_ptr(previewView));
                glUniformMatrix4fv(ctx.shader->loc("projection"), 1, GL_FALSE, glm::value_ptr(previewProj));
                glUniform3f(ctx.shader->loc("viewPos"), 3.0f, 2.0f, 3.0f);
                glUniform3f(ctx.shader->loc("lightPos"), 3.0f, 5.0f, 3.0f);

                glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(previewModel)));
                glUniformMatrix4fv(ctx.shader->loc("model"), 1, GL_FALSE, glm::value_ptr(previewModel));
                glUniformMatrix3fv(ctx.shader->loc("normalMatrix"), 1, GL_FALSE, glm::value_ptr(normalMat));

                sSlotPreviewMesh[i]->draw(*ctx.shader);

                unbindRenderTarget(ctx.width, ctx.height);
            }

            // Restore main shader state
            ctx.shader->use();
            glm::mat4 identity(1.0f);
            ctx.scene->uploadFrameUniforms(*ctx.shader, identity);
            glUniformMatrix4fv(ctx.shader->loc("view"), 1, GL_FALSE, glm::value_ptr(ctx.scene->view));
            glUniformMatrix4fv(ctx.shader->loc("projection"), 1, GL_FALSE, glm::value_ptr(ctx.scene->projection));
            Camera* cam = getGlobalCamera();
            glUniform3fv(ctx.shader->loc("viewPos"), 1, glm::value_ptr(cam->position));
            glm::vec3 lightPos = cam->position + glm::vec3(0.0f, 2.0f, 0.0f);
            glUniform3fv(ctx.shader->loc("lightPos"), 1, glm::value_ptr(lightPos));

            // Draw ghost block at origin if only ghost collider is there
            const BlockCollider* ghostCol = getColliderAt(0, 0, 0);
            if (!ghostCol || ghostCol->meshName == "_ghost") {
                ctx.shader->use();
                AABB ghostBox;
                ghostBox.min = glm::vec3(-0.5f);
                ghostBox.max = glm::vec3(0.5f);
                glm::vec3 ghostColor(0.5f);
                for (int face = 0; face < 6; face++) {
                    Triangle t0 = aabbFaceTriangle(ghostBox, face, 0);
                    Triangle t1 = aabbFaceTriangle(ghostBox, face, 1);
                    drawTriangleOverlay(*ctx.shader, t0, ghostColor, 0.15f);
                    drawTriangleOverlay(*ctx.shader, t1, ghostColor, 0.15f);
                }
            }

            if (!sPaused)
                renderHoverHighlight();

            renderUI();

            if (sEditorMode == 1 && !sPaused)
                drawColorWheel(getUIShader());
        }
    );

    // ---- AI tools ----------------------------------------------------------
    // Tools mutate scene state directly; they refuse unless the 3dModeler scene
    // is active. Each mutating tool pushes one undo step so Ctrl+Z reverts it.
    auto require3dModeler = []() {
        if (getActiveSceneName() != "3dModeler")
            throw std::runtime_error("tool requires the 3dModeler scene to be active");
    };

    AI::registerTool("set_palette_color", [require3dModeler](const AI::Json& a) -> AI::Json {
        require3dModeler();
        int slot = a.at("slot").get<int>();
        int total = (int)sColorWheel.size();
        if (slot < 0 || slot >= total)
            throw std::runtime_error("slot must be 0.." + std::to_string(total - 1));
        float r = a.at("r").get<float>();
        float g = a.at("g").get<float>();
        float b = a.at("b").get<float>();
        pushUndo();
        sColorWheel[slot] = glm::vec3(r, g, b);
        setPaintPalette(sColorWheel.data(), (int)sColorWheel.size());
        return AI::Json{{"slot", slot}, {"r", r}, {"g", g}, {"b", b}};
    });

    AI::registerTool("get_palette", [require3dModeler](const AI::Json&) -> AI::Json {
        require3dModeler();
        AI::Json arr = AI::Json::array();
        for (size_t i = 0; i < sColorWheel.size(); i++) {
            arr.push_back({sColorWheel[i].r, sColorWheel[i].g, sColorWheel[i].b});
        }
        return arr;
    });

    AI::registerTool("select_color_slot", [require3dModeler](const AI::Json& a) -> AI::Json {
        require3dModeler();
        int slot = a.at("slot").get<int>();
        int total = (int)sColorWheel.size();
        if (slot < 0 || slot >= total)
            throw std::runtime_error("slot must be 0.." + std::to_string(total - 1));
        sSelectedColor = slot;
        sPaintPage = slot / COLORS_PER_PAINT_PACK;
        return AI::Json{{"selected", slot}};
    });

    // ---- Context tools (only exposed to the model when the user clicks
    //      "Send + Info"; always registered, gating happens in agent.py) ----
    AI::registerTool("get_camera_position", [require3dModeler](const AI::Json&) -> AI::Json {
        require3dModeler();
        Camera* cam = getGlobalCamera();
        return AI::Json{
            {"x", cam->position.x}, {"y", cam->position.y}, {"z", cam->position.z},
            {"yaw", cam->yaw}, {"pitch", cam->pitch}
        };
    });

    AI::registerTool("get_camera_forward", [require3dModeler](const AI::Json&) -> AI::Json {
        require3dModeler();
        Camera* cam = getGlobalCamera();
        float yawRad = glm::radians(cam->yaw);
        float pitchRad = glm::radians(cam->pitch);
        glm::vec3 d(
            std::cos(pitchRad) * std::sin(yawRad),
            std::sin(pitchRad),
            std::cos(pitchRad) * std::cos(yawRad)
        );
        return AI::Json{{"x", d.x}, {"y", d.y}, {"z", d.z}};
    });

    AI::registerTool("list_blocks", [require3dModeler](const AI::Json&) -> AI::Json {
        require3dModeler();
        AI::Json arr = AI::Json::array();
        for (const auto& col : getAllColliders()) {
            if (col.meshName == "_ghost") continue;
            arr.push_back({
                {"x", (int)std::round(col.position.x)},
                {"y", (int)std::round(col.position.y)},
                {"z", (int)std::round(col.position.z)},
                {"mesh", col.meshName}
            });
        }
        return arr;
    });

    AI::registerTool("get_aimed_block", [require3dModeler](const AI::Json& a) -> AI::Json {
        require3dModeler();
        Camera* cam = getGlobalCamera();
        float yawRad = glm::radians(cam->yaw);
        float pitchRad = glm::radians(cam->pitch);
        glm::vec3 dir(
            std::cos(pitchRad) * std::sin(yawRad),
            std::sin(pitchRad),
            std::cos(pitchRad) * std::cos(yawRad)
        );
        float maxDist = a.value("max_distance", 50.0f);

        const auto& cols = getAllColliders();
        float bestT = std::numeric_limits<float>::max();
        int bestFace = -1;
        glm::vec3 bestPos(0.0f);
        bool found = false;

        for (const auto& col : cols) {
            if (col.meshName == "_ghost") continue;
            glm::vec3 mn = col.position - glm::vec3(0.5f);
            glm::vec3 mx = col.position + glm::vec3(0.5f);
            // Slab test: find tEnter and which face the ray entered through.
            float tmin = -std::numeric_limits<float>::infinity();
            float tmax =  std::numeric_limits<float>::infinity();
            int enterFace = -1;
            bool valid = true;
            for (int axis = 0; axis < 3; axis++) {
                if (std::fabs(dir[axis]) < 1e-8f) {
                    if (cam->position[axis] < mn[axis] || cam->position[axis] > mx[axis]) {
                        valid = false; break;
                    }
                    continue;
                }
                float invD = 1.0f / dir[axis];
                float t1 = (mn[axis] - cam->position[axis]) * invD;
                float t2 = (mx[axis] - cam->position[axis]) * invD;
                int f1 = axis * 2 + 1;  // -X / -Y / -Z (entered from negative side)
                int f2 = axis * 2;      // +X / +Y / +Z
                if (t1 > t2) { std::swap(t1, t2); std::swap(f1, f2); }
                if (t1 > tmin) { tmin = t1; enterFace = f1; }
                if (t2 < tmax) tmax = t2;
                if (tmin > tmax) { valid = false; break; }
            }
            if (!valid) continue;
            if (tmin < 0.0f || tmin > maxDist) continue;
            if (tmin < bestT) {
                bestT = tmin;
                bestFace = enterFace;
                bestPos = col.position;
                found = true;
            }
        }

        if (!found) return AI::Json{{"hit", false}};
        return AI::Json{
            {"hit", true},
            {"x", (int)std::round(bestPos.x)},
            {"y", (int)std::round(bestPos.y)},
            {"z", (int)std::round(bestPos.z)},
            {"face", bestFace},
            {"distance", bestT}
        };
    });

    AI::registerTool("edit_slot", [require3dModeler](const AI::Json& a) -> AI::Json {
        require3dModeler();
        int slot = a.at("slot").get<int>();
        if (slot < 0 || slot >= (int)sSlotMesh.size())
            throw std::runtime_error("slot out of range [0.." +
                std::to_string((int)sSlotMesh.size() - 1) + "]");
        const std::string& cur = sSlotMesh[slot];
        if (!cur.empty()) {
            const RegisteredMesh* reg = getRegisteredMesh(cur.c_str());
            if (reg && reg->isPrefab)
                throw std::runtime_error("slot '" + cur + "' is a built-in prefab and cannot be edited");
        }
        saveCurrentModel();
        sSelectedSlot = slot;
        std::string meshName = cur.empty() ? ("slot_" + std::to_string(slot)) : cur;
        sPendingSlotUpdate = slot;
        sPendingSlotMesh = meshName;
        VE::setScene("vectorMesh", std::make_shared<VectorMeshEditData>(VectorMeshEditData{meshName, slot, sModelName}));
        return AI::Json{{"slot", slot}, {"mesh_name", meshName}};
    });
}
