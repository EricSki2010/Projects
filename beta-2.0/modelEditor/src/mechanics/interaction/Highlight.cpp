#include "Highlight.h"
#include "Selection.h"
#include "../../scenes/3dModeler.h"
#include "../../../../VisualEngine/EngineGlobals.h"
#include "../../../../VisualEngine/inputManagement/Raycasting.h"
#include "../../../../VisualEngine/inputManagement/Collision.h"
#include "../../../../VisualEngine/renderingManagement/meshing/Overlay.h"
#include "../../../../VisualEngine/VisualEngine.h"
#include "../../../../VisualEngine/inputManagement/Camera.h"
#include <algorithm>
#include <cmath>

// Current mesh for placing/replacing blocks
static std::string sCurrentMesh = "cube";

// Drag state
static bool sDragging = false;
static bool sWasRightDown = false;
static bool sWasTabDown = false;
static bool sWasDDown = false;
static bool sWasADown = false;
static bool sWas1Down = false;
static bool sWas2Down = false;
static bool sWas3Down = false;
static bool sBlockSelectMode = false;
static glm::ivec3 sStartBlock;
static int sStartFace = -1;

// Paint mode drag state
static bool sPaintDragging = false;
static bool sWasLeftDown = false;
static glm::vec3 sPaintStartNormal;
static float sPaintStartPlane;  // dot(normal, point) for coplanarity check
static std::vector<SelectedFace> sPaintPreview;

static glm::vec3 getHitNormal(const BlockCollider* col, int triIndex) {
    if (col->isRectangular && triIndex >= 0 && triIndex < 6) {
        static const glm::vec3 normals[6] = {
            {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
        };
        return normals[triIndex];
    }
    if (triIndex >= 0 && triIndex < (int)col->triangles.size()) {
        const Triangle& t = col->triangles[triIndex];
        return glm::normalize(glm::cross(t.v1 - t.v0, t.v2 - t.v0));
    }
    return glm::vec3(0, 1, 0);
}

static glm::vec3 getHitCenter(const BlockCollider* col, int triIndex) {
    if (col->isRectangular && triIndex >= 0 && triIndex < 6) {
        glm::vec3 center = (col->bounds.min + col->bounds.max) * 0.5f;
        static const glm::vec3 normals[6] = {
            {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
        };
        glm::vec3 half = (col->bounds.max - col->bounds.min) * 0.5f;
        return center + normals[triIndex] * half;
    }
    if (triIndex >= 0 && triIndex < (int)col->triangles.size()) {
        const Triangle& t = col->triangles[triIndex];
        return (t.v0 + t.v1 + t.v2) / 3.0f;
    }
    return col->position;
}

static const glm::ivec3 sFaceNormals[6] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

// Get extrude direction from a selected face
// For rectangular (cube) colliders: uses the 6-face lookup
// For non-rectangular: computes from the triangle's actual normal
static glm::ivec3 getExtrudeDirection(const SelectedFace& sel) {
    const BlockCollider* col = getColliderAt(sel.blockPos.x, sel.blockPos.y, sel.blockPos.z);
    if (!col) return glm::ivec3(0);

    if ((col->isRectangular || isForceRectangularRaycast()) && sel.faceIndex >= 0 && sel.faceIndex < 6) {
        return sFaceNormals[sel.faceIndex];
    }

    // Non-rectangular: get triangle normal and snap to nearest axis
    if (sel.faceIndex >= 0 && sel.faceIndex < (int)col->triangles.size()) {
        const Triangle& tri = col->triangles[sel.faceIndex];
        glm::vec3 edge1 = tri.v1 - tri.v0;
        glm::vec3 edge2 = tri.v2 - tri.v0;
        glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));

        // Snap to nearest axis direction
        glm::ivec3 dir(0);
        int bestAxis = 0;
        float bestDot = 0.0f;
        for (int a = 0; a < 3; a++) {
            float d = std::abs(normal[a]);
            if (d > bestDot) {
                bestDot = d;
                bestAxis = a;
            }
        }
        dir[bestAxis] = (normal[bestAxis] > 0.0f) ? 1 : -1;
        return dir;
    }

    return glm::ivec3(0, 1, 0); // fallback: up
}

static void getFacePlaneAxes(int faceIndex, int& axisA, int& axisB) {
    switch (faceIndex / 2) {
        case 0: axisA = 1; axisB = 2; break;
        case 1: axisA = 0; axisB = 2; break;
        case 2: axisA = 0; axisB = 1; break;
        default: axisA = 0; axisB = 1; break;
    }
}

static int getFaceDepthAxis(int faceIndex) {
    return faceIndex / 2;
}

static void drawFaceOverlay(const glm::ivec3& blockPos, int faceIndex,
                            const glm::vec3& color, float alpha) {
    const BlockCollider* col = getColliderAt(blockPos.x, blockPos.y, blockPos.z);
    if (!col) return;

    Triangle t0 = aabbFaceTriangle(col->bounds, faceIndex, 0);
    Triangle t1 = aabbFaceTriangle(col->bounds, faceIndex, 1);
    drawTriangleOverlay(*ctx.shader, t0, color, alpha);
    drawTriangleOverlay(*ctx.shader, t1, color, alpha);
}

void setCurrentMesh(const std::string& meshName) {
    sCurrentMesh = meshName;
}

const std::string& getCurrentMesh() {
    return sCurrentMesh;
}

static void drawBlockHighlight(const glm::ivec3& pos, const glm::vec3& color, float alpha) {
    for (int face = 0; face < 6; face++) {
        glm::ivec3 n = sFaceNormals[face];
        if (VE::hasBlockAt(pos.x + n.x, pos.y + n.y, pos.z + n.z))
            continue;
        drawFaceOverlay(pos, face, color, alpha);
    }
}

// Collect all valid faces in the rectangle between start and end block on the locked plane
static void collectRectangleFaces(const glm::ivec3& startBlock, const glm::ivec3& endBlock,
                                  int faceIndex, std::vector<SelectedFace>& out) {
    int axisA, axisB;
    getFacePlaneAxes(faceIndex, axisA, axisB);
    int depthAxis = getFaceDepthAxis(faceIndex);
    int depth = startBlock[depthAxis];
    glm::ivec3 neighbor = sFaceNormals[faceIndex];

    int minA = std::min(startBlock[axisA], endBlock[axisA]);
    int maxA = std::max(startBlock[axisA], endBlock[axisA]);
    int minB = std::min(startBlock[axisB], endBlock[axisB]);
    int maxB = std::max(startBlock[axisB], endBlock[axisB]);

    for (int a = minA; a <= maxA; a++) {
        for (int b = minB; b <= maxB; b++) {
            glm::ivec3 pos;
            pos[axisA] = a;
            pos[axisB] = b;
            pos[depthAxis] = depth;

            if (!VE::hasBlockAt(pos.x, pos.y, pos.z))
                continue;
            if (VE::hasBlockAt(pos.x + neighbor.x, pos.y + neighbor.y, pos.z + neighbor.z))
                continue;

            out.push_back({pos, faceIndex});
        }
    }
}

void initHighlight() {
    initOverlay();
}

void cleanupHighlight() {
    cleanupOverlay();
}

void renderHoverHighlight() {
    // Always draw stored selection, even in movement mode
    if (sBlockSelectMode) {
        glm::vec3 purple(0.6f, 0.2f, 1.0f);
        std::vector<glm::ivec3> drawnBlocks;
        for (const auto& sel : getSelection()) {
            bool already = false;
            for (const auto& b : drawnBlocks)
                if (b == sel.blockPos) { already = true; break; }
            if (already) continue;
            drawnBlocks.push_back(sel.blockPos);
            drawBlockHighlight(sel.blockPos, purple, 0.45f);
        }
    } else if (!isForceRectangularRaycast()) {
        // Paint mode: highlight selected faces/triangles
        glm::vec3 green(0.2f, 1.0f, 0.4f);
        for (const auto& sel : getSelection()) {
            const BlockCollider* col = getColliderAt(sel.blockPos.x, sel.blockPos.y, sel.blockPos.z);
            if (!col) continue;
            if (col->isRectangular)
                drawFaceOverlay(sel.blockPos, sel.faceIndex, green, 0.45f);
            else if (sel.faceIndex < (int)col->triangles.size())
                drawTriangleOverlay(*ctx.shader, col->triangles[sel.faceIndex], green, 0.45f);
        }
    } else {
        glm::vec3 blue(0.0f, 0.5f, 1.0f);
        for (const auto& sel : getSelection())
            drawFaceOverlay(sel.blockPos, sel.faceIndex, blue, 0.45f);
    }

    // No interaction in movement mode
    if (getGlobalCamera()->looking)
        return;

    double mx, my;
    glfwGetCursorPos(ctx.window, &mx, &my);
    if (mx < 0 || mx >= ctx.width || my < 0 || my >= ctx.height)
        return;

    Ray ray = screenToRay(mx, my, ctx.width, ctx.height, ctx.scene->view, ctx.scene->projection);
    CollisionHit hit = raycast(ray.origin, ray.direction);

    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool leftJustPressed = leftDown && !sWasLeftDown;
    sWasLeftDown = leftDown;

    bool rightDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    bool rightJustPressed = rightDown && !sWasRightDown;
    bool rightJustReleased = !rightDown && sWasRightDown;
    sWasRightDown = rightDown;

    bool shiftHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    bool ctrlHeld = glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

    // Tab toggles block select mode (only when there's a selection)
    bool tabDown = glfwGetKey(ctx.window, GLFW_KEY_TAB) == GLFW_PRESS;
    if (tabDown && !sWasTabDown && !getSelection().empty())
        sBlockSelectMode = !sBlockSelectMode;
    sWasTabDown = tabDown;

    // Exit block select mode if selection is cleared
    if (getSelection().empty())
        sBlockSelectMode = false;

    // Ctrl+D deletes selected blocks (build mode only)
    bool dDown = glfwGetKey(ctx.window, GLFW_KEY_D) == GLFW_PRESS;
    if (dDown && !sWasDDown && ctrlHeld && !getSelection().empty() && isForceRectangularRaycast()) {
        pushUndoSnapshot();
        std::vector<glm::ivec3> blocks;
        for (const auto& sel : getSelection()) {
            bool already = false;
            for (const auto& b : blocks)
                if (b == sel.blockPos) { already = true; break; }
            if (!already) blocks.push_back(sel.blockPos);
        }
        clearSelection();
        sBlockSelectMode = false;
        for (const auto& pos : blocks)
            VE::undraw((float)pos.x, (float)pos.y, (float)pos.z);
    }
    sWasDDown = dDown;

    // Ctrl+A: block select mode = replace blocks, face select mode = extrude (build mode only)
    bool aDown = glfwGetKey(ctx.window, GLFW_KEY_A) == GLFW_PRESS;
    if (aDown && !sWasADown && ctrlHeld && !getSelection().empty() && !rightDown && isForceRectangularRaycast()) {
        pushUndoSnapshot();
        if (sBlockSelectMode) {
            // Replace: remove and re-place each selected block
            std::vector<glm::ivec3> blocks;
            for (const auto& sel : getSelection()) {
                bool already = false;
                for (const auto& b : blocks)
                    if (b == sel.blockPos) { already = true; break; }
                if (!already) blocks.push_back(sel.blockPos);
            }

            for (const auto& pos : blocks) {
                VE::undraw((float)pos.x, (float)pos.y, (float)pos.z);
                VE::draw(sCurrentMesh.c_str(), (float)pos.x, (float)pos.y, (float)pos.z);
            }
            clearSelection();
            sBlockSelectMode = false;
        } else {
            // Extrude from selected faces
            std::vector<SelectedFace> newFaces;
            for (const auto& sel : getSelection()) {
                glm::ivec3 n = getExtrudeDirection(sel);
                glm::ivec3 newPos = sel.blockPos + n;

                if (VE::hasBlockAt(newPos.x, newPos.y, newPos.z))
                    continue;

                VE::draw(sCurrentMesh.c_str(), (float)newPos.x, (float)newPos.y, (float)newPos.z);
                newFaces.push_back({newPos, sel.faceIndex});
            }
            clearSelection();
            for (const auto& f : newFaces)
                addSelectedFace(f.blockPos, f.faceIndex);
        }
    }
    // Ctrl+A in paint mode: color selected faces
    if (aDown && !sWasADown && ctrlHeld && !getSelection().empty() && !isForceRectangularRaycast()) {
        int colorIdx = getSelectedPaintColor();
        if (colorIdx >= 0) {
            pushUndoSnapshot();
            for (const auto& sel : getSelection()) {
                BlockCollider* col = const_cast<BlockCollider*>(
                    getColliderAt(sel.blockPos.x, sel.blockPos.y, sel.blockPos.z));
                if (col && sel.faceIndex >= 0 && sel.faceIndex < (int)col->triColors.size())
                    col->triColors[sel.faceIndex] = (int16_t)colorIdx;
            }
            clearSelection();
            ctx.needsRebuild = true;
        }
    }
    sWasADown = aDown;

    // R + 1/2/3: rotate selected blocks by 45 degrees (block select mode only)
    bool rHeld = glfwGetKey(ctx.window, GLFW_KEY_R) == GLFW_PRESS;
    bool key1 = glfwGetKey(ctx.window, GLFW_KEY_1) == GLFW_PRESS;
    bool key2 = glfwGetKey(ctx.window, GLFW_KEY_2) == GLFW_PRESS;
    bool key3 = glfwGetKey(ctx.window, GLFW_KEY_3) == GLFW_PRESS;
    if (rHeld && sBlockSelectMode && !getSelection().empty()) {
        glm::vec3 addRot(0.0f);
        bool doRotate = false;
        if (key1 && !sWas1Down) { addRot.x = 90.0f; doRotate = true; }
        if (key2 && !sWas2Down) { addRot.y = 90.0f; doRotate = true; }
        if (key3 && !sWas3Down) { addRot.z = 90.0f; doRotate = true; }

        if (doRotate) {
            pushUndoSnapshot();
            std::vector<glm::ivec3> blocks;
            for (const auto& sel : getSelection()) {
                bool already = false;
                for (const auto& b : blocks)
                    if (b == sel.blockPos) { already = true; break; }
                if (!already) blocks.push_back(sel.blockPos);
            }
            for (const auto& pos : blocks) {
                const BlockCollider* col = getColliderAt(pos.x, pos.y, pos.z);
                if (!col) continue;
                std::string mesh = col->meshName;
                glm::vec3 rot = col->rotation + addRot;
                VE::undraw((float)pos.x, (float)pos.y, (float)pos.z);
                VE::draw(mesh.c_str(), (float)pos.x, (float)pos.y, (float)pos.z,
                         rot.x, rot.y, rot.z);
            }
        }
    }
    sWas1Down = key1;
    sWas2Down = key2;
    sWas3Down = key3;

    // Paint mode interactions
    if (!isForceRectangularRaycast()) {
        // Left-click drag: brush select (immediate, paints as you drag)
        if (leftDown && hit.hit && hit.collider && hit.triangleIndex >= 0) {
            if (leftJustPressed && !shiftHeld && !ctrlHeld)
                clearSelection();
            glm::ivec3 blockPos = glm::ivec3(glm::round(hit.collider->position));
            if (ctrlHeld)
                removeSelectedFace(blockPos, hit.triangleIndex);
            else
                addSelectedFace(blockPos, hit.triangleIndex);
        }

        // Right-click: single select
        if (rightJustPressed && hit.hit && hit.collider && hit.triangleIndex >= 0) {
            if (!shiftHeld && !ctrlHeld)
                clearSelection();
            // Start plane drag
            sPaintDragging = true;
            sPaintPreview.clear();
            sPaintStartNormal = getHitNormal(hit.collider, hit.triangleIndex);
            sPaintStartPlane = glm::dot(sPaintStartNormal, getHitCenter(hit.collider, hit.triangleIndex));
        }

        // Right-click drag: plane select (preview, commit on release)
        if (sPaintDragging && rightDown && hit.hit && hit.collider && hit.triangleIndex >= 0) {
            glm::ivec3 blockPos = glm::ivec3(glm::round(hit.collider->position));
            glm::vec3 triNormal = getHitNormal(hit.collider, hit.triangleIndex);
            glm::vec3 triCenter = getHitCenter(hit.collider, hit.triangleIndex);
            float planeDist = glm::dot(sPaintStartNormal, triCenter);

            // Only select if same normal direction and on same plane
            if (glm::dot(triNormal, sPaintStartNormal) > 0.95f &&
                std::fabs(planeDist - sPaintStartPlane) < 0.1f) {
                SelectedFace f = {blockPos, hit.triangleIndex};
                bool already = false;
                for (const auto& p : sPaintPreview)
                    if (p.blockPos == f.blockPos && p.faceIndex == f.faceIndex) { already = true; break; }
                if (!already)
                    sPaintPreview.push_back(f);
            }
        }

        // Right-click release: commit plane selection
        if (rightJustReleased && sPaintDragging) {
            if (sPaintPreview.size() <= 1 && hit.hit && hit.collider && hit.triangleIndex >= 0) {
                // Single click: just select the one face
                glm::ivec3 blockPos = glm::ivec3(glm::round(hit.collider->position));
                if (ctrlHeld)
                    removeSelectedFace(blockPos, hit.triangleIndex);
                else
                    addSelectedFace(blockPos, hit.triangleIndex);
            } else {
                // Plane drag: commit all previewed faces
                for (const auto& f : sPaintPreview) {
                    if (ctrlHeld)
                        removeSelectedFace(f.blockPos, f.faceIndex);
                    else
                        addSelectedFace(f.blockPos, f.faceIndex);
                }
            }
            sPaintPreview.clear();
            sPaintDragging = false;
        }

        // Cancel drag if right released without hit
        if (rightJustReleased)
            sPaintDragging = false;
    }

    // Start drag (build mode)
    if (rightJustPressed && hit.hit && hit.collider && (hit.collider->isRectangular || isForceRectangularRaycast())) {
        sDragging = true;
        sStartBlock = glm::ivec3(glm::round(hit.collider->position));
        sStartFace = hit.triangleIndex;
        if (!shiftHeld && !ctrlHeld)
            clearSelection();
    }

    // Commit selection on release
    if (rightJustReleased && sDragging) {
        if (hit.hit && hit.collider && (hit.collider->isRectangular || isForceRectangularRaycast())) {
            glm::ivec3 endBlock = glm::ivec3(glm::round(hit.collider->position));
            std::vector<SelectedFace> faces;
            collectRectangleFaces(sStartBlock, endBlock, sStartFace, faces);
            if (ctrlHeld) {
                for (const auto& f : faces)
                    removeSelectedFace(f.blockPos, f.faceIndex);
            } else {
                for (const auto& f : faces)
                    addSelectedFace(f.blockPos, f.faceIndex);
            }
        }
        sDragging = false;
    }

    // Draw drag preview while dragging (light blue = add, red = remove)
    if (sDragging && hit.hit && hit.collider && (hit.collider->isRectangular || isForceRectangularRaycast())) {
        glm::ivec3 currentBlock = glm::ivec3(glm::round(hit.collider->position));
        std::vector<SelectedFace> preview;
        collectRectangleFaces(sStartBlock, currentBlock, sStartFace, preview);

        glm::vec3 previewColor = ctrlHeld ? glm::vec3(1.0f, 0.3f, 0.3f) : glm::vec3(0.3f, 0.7f, 1.0f);
        for (const auto& f : preview)
            drawFaceOverlay(f.blockPos, f.faceIndex, previewColor, 0.35f);
        return;
    }

    // Draw paint mode plane-drag preview
    if (sPaintDragging && !sPaintPreview.empty()) {
        glm::vec3 previewColor = ctrlHeld ? glm::vec3(1.0f, 0.3f, 0.3f) : glm::vec3(0.3f, 0.7f, 1.0f);
        for (const auto& f : sPaintPreview) {
            const BlockCollider* col = getColliderAt(f.blockPos.x, f.blockPos.y, f.blockPos.z);
            if (!col) continue;
            if (col->isRectangular)
                drawFaceOverlay(f.blockPos, f.faceIndex, previewColor, 0.35f);
            else if (f.faceIndex < (int)col->triangles.size())
                drawTriangleOverlay(*ctx.shader, col->triangles[f.faceIndex], previewColor, 0.35f);
        }
    }

    if (!hit.hit || hit.triangleIndex < 0 || !hit.collider)
        return;

    glm::vec3 yellow(1.0f, 1.0f, 0.0f);
    if ((hit.collider->isRectangular || isForceRectangularRaycast())) {
        drawFaceOverlay(glm::ivec3(glm::round(hit.collider->position)), hit.triangleIndex, yellow, 0.45f);
    } else if (hit.triangleIndex < (int)hit.collider->triangles.size()) {
        drawTriangleOverlay(*ctx.shader, hit.collider->triangles[hit.triangleIndex], yellow, 0.45f);
    }
}
