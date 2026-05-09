#include "GltfExporter.h"
#include "../../../../VisualEngine/inputManagement/Collision.h"
#include "../../../../VisualEngine/renderingManagement/meshing/ChunkMesh.h"
#include <json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <climits>
#include <limits>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>

// Single-header impl is in TexturePacking.cpp; here we just need declarations.
#include <stb_image_write.h>

using json = nlohmann::json;

static const glm::vec3 kUnpaintedColor(0.8f);

// Engine palette values are sRGB-equivalent (the editor's shader writes them
// straight to the framebuffer with no gamma conversion). glTF COLOR_0 must be
// linear, so vertex-color emit paths convert via this helper. The atlas
// (baseColorTexture) keeps raw bytes since glTF treats those as sRGB-encoded
// already, which round-trips back to the editor's appearance.
static glm::vec3 srgbToLinear(const glm::vec3& c) {
    auto f = [](float x) {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        if (x <= 0.04045f) return x / 12.92f;
        return std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    return glm::vec3(f(c.r), f(c.g), f(c.b));
}

static glm::vec3 rotatePoint(const glm::vec3& v, const glm::vec3& rotDeg) {
    if (rotDeg.x == 0.0f && rotDeg.y == 0.0f && rotDeg.z == 0.0f) return v;
    glm::mat4 m(1.0f);
    m = glm::rotate(m, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
    m = glm::rotate(m, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
    return glm::vec3(m * glm::vec4(v, 1.0f));
}

// Single output primitive: positions + normals + per-vertex colors + indices.
// Verts are emitted three-per-triangle (no shared verts) so each triangle
// can carry its own color without conflicts.
struct VertexColorPrim {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> colors;
    std::vector<uint32_t> indices;
};

// Emits a triangle with an explicit per-triangle normal. Callers are
// responsible for passing a correctly-oriented normal — either computed
// from winding (prefab cubes, consistent CCW-outward) or pulled from the
// source mesh's stored normal (vectorMesh VN files, where the save-time
// flip already accounts for the user's CW click convention).
static void addTriangle(VertexColorPrim& p,
                        const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                        const glm::vec3& normal,
                        const glm::vec3& color) {
    glm::vec3 lin = srgbToLinear(color);
    uint32_t base = (uint32_t)(p.positions.size() / 3);
    auto push = [&](const glm::vec3& v) {
        p.positions.push_back(v.x);
        p.positions.push_back(v.y);
        p.positions.push_back(v.z);
        p.normals.push_back(normal.x);
        p.normals.push_back(normal.y);
        p.normals.push_back(normal.z);
        p.colors.push_back(lin.r);
        p.colors.push_back(lin.g);
        p.colors.push_back(lin.b);
    };
    push(v0); push(v1); push(v2);
    p.indices.push_back(base);
    p.indices.push_back(base + 1);
    p.indices.push_back(base + 2);
}

static void padBuffer(std::vector<uint8_t>& buf) {
    while (buf.size() % 4 != 0) buf.push_back(0);
}

// ── Greedy meshing ─────────────────────────────────────────────────────
//
// For axis-aligned cubes on integer positions, collect each visible face
// into a "plane grid" keyed by (face direction, plane coordinate). Then
// run 2D greedy meshing per plane: walk cells in (v, u) order, find the
// largest rectangle of same-color cells starting at each unprocessed cell,
// and emit it as one merged quad (2 triangles) instead of two per cell.

// (normalAxis, uAxis, vAxis) for each face direction in cardinal order:
// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
struct FaceAxes { int normalAxis, uAxis, vAxis; };
static const FaceAxes kFaceAxes[6] = {
    {0, 1, 2}, {0, 1, 2},
    {1, 0, 2}, {1, 0, 2},
    {2, 0, 1}, {2, 0, 1},
};
static const glm::vec3 kCardinalDirsLocal[6] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};
static bool isPositiveFace(int faceDir) {
    return faceDir == 0 || faceDir == 2 || faceDir == 4;
}

struct GMPlaneKey {
    int faceDir;
    int planeIntCoord; // 2 * world_coord_along_normal + (positive ? 1 : -1)
    const RegisteredMesh* mesh; // disambiguates different rectangular types
    bool operator==(const GMPlaneKey& o) const {
        return faceDir == o.faceDir
            && planeIntCoord == o.planeIntCoord
            && mesh == o.mesh;
    }
};
struct GMPlaneKeyHash {
    size_t operator()(const GMPlaneKey& k) const {
        size_t h = std::hash<int>()(k.faceDir);
        h ^= std::hash<int>()(k.planeIntCoord) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<const void*>()(k.mesh)  + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct GMCell {
    int u, v;
    int paletteIndex; // -1 = unpainted
};

static int planeIntCoordFor(int faceDir, const glm::ivec3& pos) {
    int posAlong = pos[kFaceAxes[faceDir].normalAxis];
    return 2 * posAlong + (isPositiveFace(faceDir) ? 1 : -1);
}

// Pack two ints into a 64-bit hash key (handles negatives via two's complement).
static int64_t cellKey(int u, int v) {
    return ((int64_t)(uint32_t)u << 32) | (uint32_t)v;
}

// Emit one merged quad as 4 verts + 6 indices (2 triangles sharing the
// diagonal verts within the quad).
static void emitGreedyQuad(VertexColorPrim& prim, int faceDir, int planeIntCoord,
                           int uMin, int uMax, int vMin, int vMax,
                           const glm::vec3& color) {
    const FaceAxes& axes = kFaceAxes[faceDir];
    float planeReal = planeIntCoord * 0.5f;
    float u0 = uMin - 0.5f, u1 = uMax + 0.5f;
    float v0 = vMin - 0.5f, v1 = vMax + 0.5f;

    auto makeCorner = [&](float uu, float vv) {
        glm::vec3 p(0.0f);
        p[axes.normalAxis] = planeReal;
        p[axes.uAxis]      = uu;
        p[axes.vAxis]      = vv;
        return p;
    };
    glm::vec3 c00 = makeCorner(u0, v0);
    glm::vec3 c10 = makeCorner(u1, v0);
    glm::vec3 c11 = makeCorner(u1, v1);
    glm::vec3 c01 = makeCorner(u0, v1);

    const glm::vec3& normal = kCardinalDirsLocal[faceDir];
    glm::vec3 testCross = glm::cross(c10 - c00, c11 - c00);
    bool ccwGivesOutward = glm::dot(testCross, normal) > 0.0f;

    glm::vec3 lin = srgbToLinear(color);
    uint32_t base = (uint32_t)(prim.positions.size() / 3);
    auto pushVert = [&](const glm::vec3& v) {
        prim.positions.push_back(v.x);
        prim.positions.push_back(v.y);
        prim.positions.push_back(v.z);
        prim.normals.push_back(normal.x);
        prim.normals.push_back(normal.y);
        prim.normals.push_back(normal.z);
        prim.colors.push_back(lin.r);
        prim.colors.push_back(lin.g);
        prim.colors.push_back(lin.b);
    };

    if (ccwGivesOutward) {
        pushVert(c00); pushVert(c10); pushVert(c11); pushVert(c01);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 1);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base + 3);
    } else {
        pushVert(c00); pushVert(c11); pushVert(c10); pushVert(c01);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 1);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 3);
        prim.indices.push_back(base + 1);
    }
}

// Run greedy meshing on one plane. Returns the number of merged quads emitted.
static int greedyMeshPlane(VertexColorPrim& prim, int faceDir, int planeIntCoord,
                           const std::vector<GMCell>& cells,
                           const std::function<glm::vec3(int)>& colorForIndex) {
    if (cells.empty()) return 0;

    std::unordered_map<int64_t, int> grid;
    int uMin = INT_MAX, uMax = INT_MIN, vMin = INT_MAX, vMax = INT_MIN;
    for (const auto& c : cells) {
        grid[cellKey(c.u, c.v)] = c.paletteIndex;
        if (c.u < uMin) uMin = c.u;
        if (c.u > uMax) uMax = c.u;
        if (c.v < vMin) vMin = c.v;
        if (c.v > vMax) vMax = c.v;
    }

    std::unordered_set<int64_t> processed;
    int quadsEmitted = 0;

    for (int v = vMin; v <= vMax; v++) {
        for (int u = uMin; u <= uMax; u++) {
            int64_t k = cellKey(u, v);
            if (processed.count(k)) continue;
            auto it = grid.find(k);
            if (it == grid.end()) continue;
            int paletteIndex = it->second;

            // Extend width: consecutive cells at v, same color, not processed.
            int wMax = 0;
            for (int w = 0; u + w <= uMax; w++) {
                int64_t kw = cellKey(u + w, v);
                if (processed.count(kw)) break;
                auto it2 = grid.find(kw);
                if (it2 == grid.end() || it2->second != paletteIndex) break;
                wMax = w + 1;
            }

            // Extend height: rows where the full w-wide strip matches.
            int hMax = 1;
            for (int h = 1; v + h <= vMax; h++) {
                bool rowOk = true;
                for (int w = 0; w < wMax; w++) {
                    int64_t khw = cellKey(u + w, v + h);
                    if (processed.count(khw)) { rowOk = false; break; }
                    auto it2 = grid.find(khw);
                    if (it2 == grid.end() || it2->second != paletteIndex) {
                        rowOk = false; break;
                    }
                }
                if (!rowOk) break;
                hMax = h + 1;
            }

            for (int h = 0; h < hMax; h++)
                for (int w = 0; w < wMax; w++)
                    processed.insert(cellKey(u + w, v + h));

            emitGreedyQuad(prim, faceDir, planeIntCoord,
                           u, u + wMax - 1, v, v + hMax - 1,
                           colorForIndex(paletteIndex));
            quadsEmitted++;
        }
    }
    return quadsEmitted;
}

// ── Hybrid texture/vertex-color export ─────────────────────────────────
//
// After phase 1 collects each plane's cells, we run an "aggressive" merge
// that ignores color (extending merges through any cell that exists). For
// each resulting rect:
//
//   - uniform color  → one vertex-colored quad (cheapest path)
//   - multi-color, area >= kBakeAreaThreshold
//                    → bake per-cell colors into a patch in a global atlas;
//                      emit one textured quad referencing the patch
//   - multi-color, area <  kBakeAreaThreshold
//                    → fall back to color-aware greedy on those cells
//                      (small per-color rects with vertex colors)

static constexpr int kBakeAreaThreshold = 16; // cells; tune for atlas/tri tradeoff
static constexpr int kAtlasPadding      = 1;  // texel gutter around each patch

struct AggressiveRect {
    int uMin, uMax, vMin, vMax;
    std::vector<int> cellPalette; // (uMax-uMin+1)*(vMax-vMin+1), v-major; -1 = unpainted
    bool uniformColor       = false;
    int  uniformPaletteIndex = -1;
};

static std::vector<AggressiveRect> aggressiveMeshPlane(const std::vector<GMCell>& cells) {
    std::vector<AggressiveRect> rects;
    if (cells.empty()) return rects;

    std::unordered_map<int64_t, int> grid;
    int uMin = INT_MAX, uMax = INT_MIN, vMin = INT_MAX, vMax = INT_MIN;
    for (const auto& c : cells) {
        grid[cellKey(c.u, c.v)] = c.paletteIndex;
        if (c.u < uMin) uMin = c.u;
        if (c.u > uMax) uMax = c.u;
        if (c.v < vMin) vMin = c.v;
        if (c.v > vMax) vMax = c.v;
    }
    std::unordered_set<int64_t> processed;

    for (int v = vMin; v <= vMax; v++) {
        for (int u = uMin; u <= uMax; u++) {
            int64_t k = cellKey(u, v);
            if (processed.count(k)) continue;
            if (!grid.count(k)) continue;

            // Existence-only width extension.
            int wMax = 0;
            for (int w = 0; u + w <= uMax; w++) {
                int64_t kw = cellKey(u + w, v);
                if (processed.count(kw) || !grid.count(kw)) break;
                wMax = w + 1;
            }
            // Existence-only height extension.
            int hMax = 1;
            for (int h = 1; v + h <= vMax; h++) {
                bool rowOk = true;
                for (int w = 0; w < wMax; w++) {
                    int64_t khw = cellKey(u + w, v + h);
                    if (processed.count(khw) || !grid.count(khw)) { rowOk = false; break; }
                }
                if (!rowOk) break;
                hMax = h + 1;
            }

            for (int h = 0; h < hMax; h++)
                for (int w = 0; w < wMax; w++)
                    processed.insert(cellKey(u + w, v + h));

            AggressiveRect r;
            r.uMin = u;       r.uMax = u + wMax - 1;
            r.vMin = v;       r.vMax = v + hMax - 1;
            r.cellPalette.resize(wMax * hMax);
            int firstColor = grid[cellKey(u, v)];
            bool uniform = true;
            for (int h = 0; h < hMax; h++) {
                for (int w = 0; w < wMax; w++) {
                    int c = grid[cellKey(u + w, v + h)];
                    r.cellPalette[h * wMax + w] = c;
                    if (c != firstColor) uniform = false;
                }
            }
            r.uniformColor        = uniform;
            r.uniformPaletteIndex = uniform ? firstColor : -1;
            rects.push_back(std::move(r));
        }
    }
    return rects;
}

struct BakeRegion {
    int faceDir;
    int planeIntCoord;
    int uMin, uMax, vMin, vMax;
    int width  = 0; // = uMax-uMin+1
    int height = 0; // = vMax-vMin+1
    std::vector<int> cellPalette;
    int atlasX = 0, atlasY = 0;
};

// Vertex+UV primitive (no per-vertex colors). Verts are emitted four-per-quad
// (no shared verts across textured quads) so each textured quad keeps its own
// UV span without conflicts.
struct TexturedPrim {
    std::vector<float>    positions;
    std::vector<float>    normals;
    std::vector<float>    uvs;
    std::vector<uint32_t> indices;
};

static void emitTexturedQuad(TexturedPrim& prim, int faceDir, int planeIntCoord,
                             int uMin, int uMax, int vMin, int vMax,
                             int atlasX, int atlasY, int regionW, int regionH,
                             int atlasW, int atlasH) {
    const FaceAxes& axes = kFaceAxes[faceDir];
    float planeReal = planeIntCoord * 0.5f;
    float u0 = uMin - 0.5f, u1 = uMax + 0.5f;
    float v0 = vMin - 0.5f, v1 = vMax + 0.5f;

    auto makeCorner = [&](float uu, float vv) {
        glm::vec3 p(0.0f);
        p[axes.normalAxis] = planeReal;
        p[axes.uAxis]      = uu;
        p[axes.vAxis]      = vv;
        return p;
    };
    glm::vec3 c00 = makeCorner(u0, v0);
    glm::vec3 c10 = makeCorner(u1, v0);
    glm::vec3 c11 = makeCorner(u1, v1);
    glm::vec3 c01 = makeCorner(u0, v1);

    // glTF UVs are upper-left origin; we wrote the patch top-down so atlasY=0
    // is the top row. The world-vMin corner therefore maps to atlasV0.
    float au0 = (float)atlasX             / (float)atlasW;
    float au1 = (float)(atlasX + regionW) / (float)atlasW;
    float av0 = (float)atlasY             / (float)atlasH;
    float av1 = (float)(atlasY + regionH) / (float)atlasH;

    const glm::vec3& normal = kCardinalDirsLocal[faceDir];
    glm::vec3 testCross = glm::cross(c10 - c00, c11 - c00);
    bool ccwGivesOutward = glm::dot(testCross, normal) > 0.0f;

    uint32_t base = (uint32_t)(prim.positions.size() / 3);
    auto pushVert = [&](const glm::vec3& p, float uu, float vv) {
        prim.positions.push_back(p.x);
        prim.positions.push_back(p.y);
        prim.positions.push_back(p.z);
        prim.normals.push_back(normal.x);
        prim.normals.push_back(normal.y);
        prim.normals.push_back(normal.z);
        prim.uvs.push_back(uu);
        prim.uvs.push_back(vv);
    };

    if (ccwGivesOutward) {
        pushVert(c00, au0, av0);
        pushVert(c10, au1, av0);
        pushVert(c11, au1, av1);
        pushVert(c01, au0, av1);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 1);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base + 3);
    } else {
        pushVert(c00, au0, av0);
        pushVert(c11, au1, av1);
        pushVert(c10, au1, av0);
        pushVert(c01, au0, av1);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 1);
        prim.indices.push_back(base + 2);
        prim.indices.push_back(base);
        prim.indices.push_back(base + 3);
        prim.indices.push_back(base + 1);
    }
}

// Shelf-pack the regions into an atlas. Updates atlasX/atlasY on each region.
struct AtlasSize { int w, h; };
static AtlasSize packAtlas(std::vector<BakeRegion>& regions) {
    if (regions.empty()) return {0, 0};

    std::vector<BakeRegion*> sorted;
    sorted.reserve(regions.size());
    for (auto& r : regions) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [](BakeRegion* a, BakeRegion* b) {
        return a->height > b->height;
    });

    int maxPadW = kAtlasPadding * 2;
    int totalArea = 0;
    for (auto* r : sorted) {
        int padW = r->width  + kAtlasPadding * 2;
        int padH = r->height + kAtlasPadding * 2;
        if (padW > maxPadW) maxPadW = padW;
        totalArea += padW * padH;
    }
    int width = std::max(maxPadW, (int)std::ceil(std::sqrt((double)totalArea)));
    width = ((width + 15) / 16) * 16;

    int curX = kAtlasPadding;
    int curY = kAtlasPadding;
    int rowH = 0;
    for (auto* r : sorted) {
        int padW = r->width + kAtlasPadding;
        if (curX + padW > width) {
            curX = kAtlasPadding;
            curY += rowH + kAtlasPadding;
            rowH = 0;
        }
        r->atlasX = curX;
        r->atlasY = curY;
        curX += padW;
        if (r->height > rowH) rowH = r->height;
    }
    int height = curY + rowH + kAtlasPadding;
    height = ((height + 15) / 16) * 16;
    return { width, height };
}

// Render baked regions into an RGBA atlas, with edge-replication padding so
// any non-NEAREST sampling at patch borders doesn't bleed in transparent
// gutter pixels.
static std::vector<uint8_t> renderAtlas(const std::vector<BakeRegion>& regions,
                                        int atlasW, int atlasH,
                                        const std::function<glm::vec3(int)>& colorForIndex) {
    std::vector<uint8_t> pixels((size_t)atlasW * atlasH * 4, 0);

    auto setPx = [&](int x, int y, const glm::vec3& c) {
        if (x < 0 || x >= atlasW || y < 0 || y >= atlasH) return;
        size_t idx = (size_t)(y * atlasW + x) * 4;
        pixels[idx + 0] = (uint8_t)(std::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        pixels[idx + 1] = (uint8_t)(std::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        pixels[idx + 2] = (uint8_t)(std::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        pixels[idx + 3] = 255;
    };
    auto copyPx = [&](int sx, int sy, int dx, int dy) {
        if (sx < 0 || sx >= atlasW || sy < 0 || sy >= atlasH) return;
        if (dx < 0 || dx >= atlasW || dy < 0 || dy >= atlasH) return;
        size_t s = (size_t)(sy * atlasW + sx) * 4;
        size_t d = (size_t)(dy * atlasW + dx) * 4;
        std::memcpy(&pixels[d], &pixels[s], 4);
    };

    for (const auto& r : regions) {
        for (int hh = 0; hh < r.height; hh++) {
            for (int ww = 0; ww < r.width; ww++) {
                int idx = hh * r.width + ww;
                glm::vec3 c = colorForIndex(r.cellPalette[idx]);
                setPx(r.atlasX + ww, r.atlasY + hh, c);
            }
        }
        // Edge-replicate padding.
        for (int p = 1; p <= kAtlasPadding; p++) {
            for (int ww = 0; ww < r.width; ww++) {
                int sx = r.atlasX + ww;
                copyPx(sx, r.atlasY,                sx, r.atlasY - p);
                copyPx(sx, r.atlasY + r.height - 1, sx, r.atlasY + r.height - 1 + p);
            }
            for (int hh = -p; hh < r.height + p; hh++) {
                int sy = r.atlasY + hh;
                copyPx(r.atlasX,               sy, r.atlasX - p,               sy);
                copyPx(r.atlasX + r.width - 1, sy, r.atlasX + r.width - 1 + p, sy);
            }
        }
    }
    return pixels;
}

// stbi_write callback: append bytes into a vector<uint8_t>.
static void atlasPNGAppend(void* context, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}

bool exportModelToGlb(const std::string& outPath) {
    const auto& colliders = getAllColliders();
    const glm::vec3* palette = getPaintPalette();
    int paletteCount = getPaintPaletteCount();

    // Shared face-pair cull set — same logic the editor renderer uses.
    FaceCullSet cullSet = computeFaceCullSet();

    VertexColorPrim prim;

    // Guard both ends so a corrupt triColors entry or a stray non-rectangular
    // triangle index can't walk past the buffer.
    auto colorForIndex = [&](int paletteIndex) -> glm::vec3 {
        if (!palette || paletteIndex < 0 || paletteIndex >= paletteCount) return kUnpaintedColor;
        return palette[paletteIndex];
    };

    int culledTris = 0;
    int emittedTris = 0;
    int greedyQuads = 0;

    // Greedy meshing hardcodes ±0.5 cell extents, so we need to verify the
    // mesh lives inside a 1x1x1 reference cube. We only require the AABB to
    // *fit inside* [-0.5, 0.5] on every axis — not to span it. That way a
    // vectorMesh that's just one flat face (e.g. a +Z quad where every vert
    // has z=0.5) still qualifies; faceCacheFor re-verifies each face sits at
    // exactly ±0.5 before admitting it to greedy, so there's no cell overlap
    // with neighbors. Cached per-mesh.
    std::unordered_map<const RegisteredMesh*, bool> fitsUnitCubeCache;
    auto fitsUnitCube = [&](const RegisteredMesh* reg) -> bool {
        auto it = fitsUnitCubeCache.find(reg);
        if (it != fitsUnitCubeCache.end()) return it->second;
        int fpv = reg->floatsPerVertex;
        const float eps = 0.01f;
        bool fits = true;
        for (int i = 0; i < reg->vertexCount && fits; i++) {
            float x = reg->vertices[i * fpv + 0];
            float y = reg->vertices[i * fpv + 1];
            float z = reg->vertices[i * fpv + 2];
            if (x < -0.5f - eps || x > 0.5f + eps ||
                y < -0.5f - eps || y > 0.5f + eps ||
                z < -0.5f - eps || z > 0.5f + eps) fits = false;
        }
        fitsUnitCubeCache[reg] = fits;
        return fits;
    };

    // Per-mesh, per-local-face eligibility for greedy meshing.
    //   rectangular meshes: all 6 faces eligible by construction (24 verts on
    //     the AABB, triColors[f] gives the face color).
    //   vectorMesh meshes: a face is eligible only if its tagged triangles
    //     (triFaceDir == f) strictly tile the 1x1 square at ±0.5. faceState==2
    //     alone is insufficient — it's an area heuristic that tolerates small
    //     gaps/overhang, which would cause greedy to paint surface the source
    //     didn't have. So we re-verify: every face-tri vert must sit on the
    //     ±0.5 plane AND inside the unit face bounds, and the summed projected
    //     area must fill the square.
    struct MeshFaceCache {
        bool eligible[6] = {false, false, false, false, false, false};
        std::vector<int> faceTris[6]; // local tri indices per face (vectorMesh only)
    };
    std::unordered_map<const RegisteredMesh*, MeshFaceCache> meshFaceCache;
    auto faceCacheFor = [&](const RegisteredMesh* reg) -> const MeshFaceCache& {
        auto it = meshFaceCache.find(reg);
        if (it != meshFaceCache.end()) return it->second;
        MeshFaceCache mc;
        if (reg->rectangular) {
            for (int i = 0; i < 6; i++) mc.eligible[i] = true;
        } else {
            int fpv = reg->floatsPerVertex;
            int triCount = reg->indexCount / 3;
            const float eps = 0.01f;
            for (int f = 0; f < 6; f++) {
                if (reg->faceState[f] != 2) continue; // not solid per editor heuristic
                const FaceAxes& axes = kFaceAxes[f];
                float expectedN = isPositiveFace(f) ? 0.5f : -0.5f;
                bool ok = true;
                float area = 0.0f;
                std::vector<int> tris;
                for (int t = 0; t < triCount && ok; t++) {
                    int fd = (t < (int)reg->triFaceDir.size()) ? reg->triFaceDir[t] : -1;
                    if (fd != f) continue;
                    uint32_t i0 = reg->indices[t * 3];
                    uint32_t i1 = reg->indices[t * 3 + 1];
                    uint32_t i2 = reg->indices[t * 3 + 2];
                    glm::vec3 verts[3] = {
                        {reg->vertices[i0 * fpv], reg->vertices[i0 * fpv + 1], reg->vertices[i0 * fpv + 2]},
                        {reg->vertices[i1 * fpv], reg->vertices[i1 * fpv + 1], reg->vertices[i1 * fpv + 2]},
                        {reg->vertices[i2 * fpv], reg->vertices[i2 * fpv + 1], reg->vertices[i2 * fpv + 2]},
                    };
                    for (int k = 0; k < 3; k++) {
                        if (std::fabs(verts[k][axes.normalAxis] - expectedN) > eps) { ok = false; break; }
                        float uu = verts[k][axes.uAxis], vv = verts[k][axes.vAxis];
                        if (uu < -0.5f - eps || uu > 0.5f + eps) { ok = false; break; }
                        if (vv < -0.5f - eps || vv > 0.5f + eps) { ok = false; break; }
                    }
                    if (!ok) break;
                    float u0 = verts[0][axes.uAxis], u1 = verts[1][axes.uAxis], u2 = verts[2][axes.uAxis];
                    float w0 = verts[0][axes.vAxis], w1 = verts[1][axes.vAxis], w2 = verts[2][axes.vAxis];
                    area += 0.5f * std::fabs((u1 - u0) * (w2 - w0) - (u2 - u0) * (w1 - w0));
                    tris.push_back(t);
                }
                // Require full unit coverage (area >= 1.0). Overlapping tris that
                // sum past 1.0 are harmless — the merged quad covers the same
                // visible surface. Gaps (area < 1.0) would paint extra surface.
                if (ok && !tris.empty() && area >= 1.0f - eps) {
                    mc.eligible[f] = true;
                    mc.faceTris[f] = std::move(tris);
                }
            }
        }
        return meshFaceCache.emplace(reg, std::move(mc)).first->second;
    };

    // ── PHASE 1: Collect face cells from greedy-eligible faces ──
    // Collider-level gate: unit-cube extents, 90-aligned rotation, integer
    // grid position. Within an eligible collider, each local face admits
    // independently — vectorMesh cubes with some multi-color or slanted
    // faces still get partial greedy merging.
    std::unordered_map<GMPlaneKey, std::vector<GMCell>, GMPlaneKeyHash> planeGrids;
    std::vector<std::vector<bool>> triConsumed(colliders.size());

    for (size_t ci = 0; ci < colliders.size(); ci++) {
        const auto& col = colliders[ci];
        if (col.meshName == "_ghost") continue;
        const RegisteredMesh* reg = getRegisteredMesh(col.meshName.c_str());
        if (!reg) continue;

        glm::ivec3 ipos((int)roundf(col.position.x),
                        (int)roundf(col.position.y),
                        (int)roundf(col.position.z));
        int rotMap[6];
        if (!fitsUnitCube(reg)) continue;
        if (!buildFaceRotMap(col.rotation, rotMap)) continue;
        if (std::fabs(col.position.x - ipos.x) > 0.01f ||
            std::fabs(col.position.y - ipos.y) > 0.01f ||
            std::fabs(col.position.z - ipos.z) > 0.01f) continue;

        const MeshFaceCache& mc = faceCacheFor(reg);
        int triCount = reg->indexCount / 3;
        triConsumed[ci].assign(triCount, false);

        for (int f = 0; f < 6; f++) {
            if (!mc.eligible[f]) continue;

            // Resolve a single palette index for this face.
            int paletteIndex = -1;
            if (reg->rectangular) {
                if (f < (int)col.triColors.size())
                    paletteIndex = col.triColors[f];
            } else {
                // All face-tris must share a color; otherwise the face can't
                // collapse to one quad and falls through to phase 3.
                bool hasColor = false;
                bool conflict = false;
                int shared = -1;
                for (int t : mc.faceTris[f]) {
                    int c = (t < (int)col.triColors.size()) ? col.triColors[t] : -1;
                    if (!hasColor) { shared = c; hasColor = true; }
                    else if (c != shared) { conflict = true; break; }
                }
                if (conflict) continue;
                paletteIndex = shared;
            }

            int worldFd = rotMap[f];
            int faceTriCount = reg->rectangular ? 2 : (int)mc.faceTris[f].size();
            if (cullSet.count({ipos, worldFd})) {
                culledTris += faceTriCount;
            } else {
                GMPlaneKey key{worldFd, planeIntCoordFor(worldFd, ipos), reg};
                int u = ipos[kFaceAxes[worldFd].uAxis];
                int v = ipos[kFaceAxes[worldFd].vAxis];
                planeGrids[key].push_back({u, v, paletteIndex});
            }

            // Mark consumed either way — phase 3 must not re-emit these tris.
            if (!reg->rectangular) {
                for (int t : mc.faceTris[f]) triConsumed[ci][t] = true;
            }
        }

        // Rectangular prefabs from registerMesh don't populate triFaceDir, so
        // we can't map tris-to-faces per-tri. But all 12 tris belong to some
        // face, all 6 faces are eligible, all were either emitted or culled —
        // so it's safe to mark the entire collider consumed.
        if (reg->rectangular) {
            for (int t = 0; t < triCount; t++) triConsumed[ci][t] = true;
        }
    }

    // ── PHASE 2: Aggressive merge → split by area → color-greedy / bake ──
    // Each plane is first merged ignoring color. The merged rects then route:
    //   uniform-color   → emit one vertex-colored quad
    //   multi-color big → queue for atlas baking, emit textured quad later
    //   multi-color sml → fall back to color-aware greedy on those cells.
    TexturedPrim texPrim;
    std::vector<BakeRegion> bakeRegions;
    int bakedQuads = 0;

    for (auto& kv : planeGrids) {
        int faceDir    = kv.first.faceDir;
        int planeCoord = kv.first.planeIntCoord;
        auto rects = aggressiveMeshPlane(kv.second);

        for (auto& r : rects) {
            int width  = r.uMax - r.uMin + 1;
            int height = r.vMax - r.vMin + 1;
            int area   = width * height;

            if (r.uniformColor) {
                emitGreedyQuad(prim, faceDir, planeCoord,
                               r.uMin, r.uMax, r.vMin, r.vMax,
                               colorForIndex(r.uniformPaletteIndex));
                greedyQuads++;
                emittedTris += 2;
            } else if (area >= kBakeAreaThreshold) {
                BakeRegion br;
                br.faceDir       = faceDir;
                br.planeIntCoord = planeCoord;
                br.uMin = r.uMin; br.uMax = r.uMax;
                br.vMin = r.vMin; br.vMax = r.vMax;
                br.width = width; br.height = height;
                br.cellPalette = std::move(r.cellPalette);
                bakeRegions.push_back(std::move(br));
            } else {
                std::vector<GMCell> sub;
                sub.reserve(area);
                for (int hh = 0; hh < height; hh++) {
                    for (int ww = 0; ww < width; ww++) {
                        sub.push_back({r.uMin + ww, r.vMin + hh,
                                       r.cellPalette[hh * width + ww]});
                    }
                }
                int q = greedyMeshPlane(prim, faceDir, planeCoord, sub, colorForIndex);
                greedyQuads += q;
                emittedTris += q * 2;
            }
        }
    }

    // ── Pack atlas + emit textured quads ──
    AtlasSize atlas = packAtlas(bakeRegions);
    std::vector<uint8_t> atlasPixels;
    std::vector<uint8_t> atlasPNG;
    if (!bakeRegions.empty()) {
        atlasPixels = renderAtlas(bakeRegions, atlas.w, atlas.h, colorForIndex);
        int ok = stbi_write_png_to_func(atlasPNGAppend, &atlasPNG,
                                        atlas.w, atlas.h, 4,
                                        atlasPixels.data(), atlas.w * 4);
        if (!ok) atlasPNG.clear();
        for (const auto& r : bakeRegions) {
            emitTexturedQuad(texPrim, r.faceDir, r.planeIntCoord,
                             r.uMin, r.uMax, r.vMin, r.vMax,
                             r.atlasX, r.atlasY, r.width, r.height,
                             atlas.w, atlas.h);
            bakedQuads++;
            emittedTris += 2;
        }
    }

    // ── PHASE 3: Per-triangle emit for everything greedy didn't consume ──
    // (wedges, slanted vectorMesh interiors, multi-color faces, rotated/off-grid
    // cubes, non-unit rectangular meshes)
    for (size_t ci = 0; ci < colliders.size(); ci++) {
        const auto& col = colliders[ci];
        if (col.meshName == "_ghost") continue;
        const RegisteredMesh* reg = getRegisteredMesh(col.meshName.c_str());
        if (!reg) continue;

        glm::vec3 pos = col.position;
        glm::vec3 rot = col.rotation;
        int fpv = reg->floatsPerVertex;
        glm::ivec3 ipos((int)roundf(pos.x), (int)roundf(pos.y), (int)roundf(pos.z));

        int rotMap[6];
        bool is90 = buildFaceRotMap(rot, rotMap);
        const auto& consumed = triConsumed[ci]; // empty if collider skipped phase 1

        int triCount = reg->indexCount / 3;
        for (int t = 0; t < triCount; t++) {
            if (t < (int)consumed.size() && consumed[t]) continue;
            uint32_t i0 = reg->indices[t * 3];
            uint32_t i1 = reg->indices[t * 3 + 1];
            uint32_t i2 = reg->indices[t * 3 + 2];

            int fd = (t < (int)reg->triFaceDir.size()) ? reg->triFaceDir[t] : -1;
            int worldFd = (is90 && fd >= 0 && fd < 6) ? rotMap[fd] : fd;

            if (worldFd >= 0 && worldFd < 6) {
                if (cullSet.count({ipos, worldFd})) {
                    culledTris++;
                    continue;
                }
            }

            int paletteIndex = -1;
            if (reg->rectangular) {
                if (fd >= 0 && fd < (int)col.triColors.size())
                    paletteIndex = col.triColors[fd];
            } else {
                if (t < (int)col.triColors.size())
                    paletteIndex = col.triColors[t];
            }
            glm::vec3 color = colorForIndex(paletteIndex);

            glm::vec3 v0(reg->vertices[i0 * fpv],
                         reg->vertices[i0 * fpv + 1],
                         reg->vertices[i0 * fpv + 2]);
            glm::vec3 v1(reg->vertices[i1 * fpv],
                         reg->vertices[i1 * fpv + 1],
                         reg->vertices[i1 * fpv + 2]);
            glm::vec3 v2(reg->vertices[i2 * fpv],
                         reg->vertices[i2 * fpv + 1],
                         reg->vertices[i2 * fpv + 2]);
            v0 = rotatePoint(v0, rot) + pos;
            v1 = rotatePoint(v1, rot) + pos;
            v2 = rotatePoint(v2, rot) + pos;

            glm::vec3 normal;
            if (fpv == 8) {
                glm::vec3 localN(reg->vertices[i0 * fpv + 5],
                                 reg->vertices[i0 * fpv + 6],
                                 reg->vertices[i0 * fpv + 7]);
                normal = glm::normalize(rotatePoint(localN, rot));
            } else {
                normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
            }

            addTriangle(prim, v0, v1, v2, normal, color);
            emittedTris++;
        }
    }

    bool hasVc  = !prim.positions.empty();
    bool hasTex = !texPrim.positions.empty() && !atlasPNG.empty();
    if (!hasVc && !hasTex) {
        std::cerr << "[gltf] Nothing to export" << std::endl;
        return false;
    }

    uint32_t vcVertCount  = hasVc  ? (uint32_t)(prim.positions.size()    / 3) : 0;
    uint32_t vcIdxCount   = hasVc  ? (uint32_t)prim.indices.size()             : 0;
    uint32_t texVertCount = hasTex ? (uint32_t)(texPrim.positions.size() / 3) : 0;
    uint32_t texIdxCount  = hasTex ? (uint32_t)texPrim.indices.size()          : 0;

    glm::vec3 vcMin( std::numeric_limits<float>::max());
    glm::vec3 vcMax(-std::numeric_limits<float>::max());
    glm::vec3 texMin( std::numeric_limits<float>::max());
    glm::vec3 texMax(-std::numeric_limits<float>::max());
    for (uint32_t i = 0; i < vcVertCount; i++) {
        glm::vec3 v(prim.positions[i * 3], prim.positions[i * 3 + 1], prim.positions[i * 3 + 2]);
        vcMin = glm::min(vcMin, v); vcMax = glm::max(vcMax, v);
    }
    for (uint32_t i = 0; i < texVertCount; i++) {
        glm::vec3 v(texPrim.positions[i * 3], texPrim.positions[i * 3 + 1], texPrim.positions[i * 3 + 2]);
        texMin = glm::min(texMin, v); texMax = glm::max(texMax, v);
    }

    // Build the binary buffer.
    std::vector<uint8_t> binBuffer;
    struct BufferView {
        size_t offset;
        size_t length;
        int target; // 34962=ARRAY_BUFFER, 34963=ELEMENT_ARRAY_BUFFER, 0=other (image)
    };
    std::vector<BufferView> bufferViews;

    auto appendBufferView = [&](const void* src, size_t bytes, int target) -> int {
        BufferView bv;
        bv.offset = binBuffer.size();
        bv.length = bytes;
        bv.target = target;
        size_t oldSize = binBuffer.size();
        binBuffer.resize(oldSize + bytes);
        std::memcpy(binBuffer.data() + oldSize, src, bytes);
        padBuffer(binBuffer);
        int viewIndex = (int)bufferViews.size();
        bufferViews.push_back(bv);
        return viewIndex;
    };

    int vcPosView = -1, vcNormView = -1, vcColorView = -1, vcIdxView = -1;
    if (hasVc) {
        vcPosView   = appendBufferView(prim.positions.data(),
                                       prim.positions.size() * sizeof(float), 34962);
        vcNormView  = appendBufferView(prim.normals.data(),
                                       prim.normals.size() * sizeof(float),   34962);
        vcColorView = appendBufferView(prim.colors.data(),
                                       prim.colors.size() * sizeof(float),    34962);
        vcIdxView   = appendBufferView(prim.indices.data(),
                                       prim.indices.size() * sizeof(uint32_t), 34963);
    }

    int texPosView = -1, texNormView = -1, texUvView = -1, texIdxView = -1, atlasImgView = -1;
    if (hasTex) {
        texPosView  = appendBufferView(texPrim.positions.data(),
                                       texPrim.positions.size() * sizeof(float), 34962);
        texNormView = appendBufferView(texPrim.normals.data(),
                                       texPrim.normals.size() * sizeof(float),   34962);
        texUvView   = appendBufferView(texPrim.uvs.data(),
                                       texPrim.uvs.size() * sizeof(float),       34962);
        texIdxView  = appendBufferView(texPrim.indices.data(),
                                       texPrim.indices.size() * sizeof(uint32_t), 34963);
        atlasImgView = appendBufferView(atlasPNG.data(), atlasPNG.size(), 0);
    }

    // Build glTF JSON
    json gltf;
    gltf["asset"]  = { {"version", "2.0"}, {"generator", "modelEditor"} };
    gltf["scene"]  = 0;
    gltf["scenes"] = json::array({ {{"nodes", json::array({0})}} });
    gltf["nodes"]  = json::array({ {{"mesh", 0}} });

    // Materials.
    json materials = json::array();
    int vcMatIndex = -1, texMatIndex = -1;
    if (hasVc) {
        json m;
        m["name"] = "vertexColored";
        m["pbrMetallicRoughness"] = {
            {"baseColorFactor", json::array({1.0f, 1.0f, 1.0f, 1.0f})},
            {"metallicFactor", 0.0f},
            {"roughnessFactor", 1.0f}
        };
        m["doubleSided"] = true;
        m["extensions"]  = { {"KHR_materials_unlit", json::object()} };
        vcMatIndex = (int)materials.size();
        materials.push_back(m);
    }
    if (hasTex) {
        json m;
        m["name"] = "bakedAtlas";
        m["pbrMetallicRoughness"] = {
            {"baseColorTexture", {{"index", 0}, {"texCoord", 0}}},
            {"metallicFactor", 0.0f},
            {"roughnessFactor", 1.0f}
        };
        m["doubleSided"] = true;
        m["extensions"]  = { {"KHR_materials_unlit", json::object()} };
        texMatIndex = (int)materials.size();
        materials.push_back(m);
    }
    gltf["materials"]      = materials;
    gltf["extensionsUsed"] = json::array({"KHR_materials_unlit"});

    // BufferViews
    json bufferViewsJson = json::array();
    for (const auto& bv : bufferViews) {
        json j = {
            {"buffer", 0},
            {"byteOffset", bv.offset},
            {"byteLength", bv.length}
        };
        if (bv.target != 0) j["target"] = bv.target;
        bufferViewsJson.push_back(j);
    }
    gltf["bufferViews"] = bufferViewsJson;

    // Accessors.
    json accessors = json::array();
    int vcPosAcc = -1, vcNormAcc = -1, vcColorAcc = -1, vcIdxAcc = -1;
    if (hasVc) {
        vcPosAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", vcPosView},
            {"componentType", 5126},
            {"count", vcVertCount},
            {"type", "VEC3"},
            {"min", json::array({vcMin.x, vcMin.y, vcMin.z})},
            {"max", json::array({vcMax.x, vcMax.y, vcMax.z})}
        });
        vcNormAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", vcNormView}, {"componentType", 5126},
            {"count", vcVertCount}, {"type", "VEC3"}
        });
        vcColorAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", vcColorView}, {"componentType", 5126},
            {"count", vcVertCount}, {"type", "VEC3"}
        });
        vcIdxAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", vcIdxView}, {"componentType", 5125},
            {"count", vcIdxCount}, {"type", "SCALAR"}
        });
    }

    int texPosAcc = -1, texNormAcc = -1, texUvAcc = -1, texIdxAcc = -1;
    if (hasTex) {
        texPosAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", texPosView},
            {"componentType", 5126},
            {"count", texVertCount},
            {"type", "VEC3"},
            {"min", json::array({texMin.x, texMin.y, texMin.z})},
            {"max", json::array({texMax.x, texMax.y, texMax.z})}
        });
        texNormAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", texNormView}, {"componentType", 5126},
            {"count", texVertCount}, {"type", "VEC3"}
        });
        texUvAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", texUvView}, {"componentType", 5126},
            {"count", texVertCount}, {"type", "VEC2"}
        });
        texIdxAcc = (int)accessors.size();
        accessors.push_back({
            {"bufferView", texIdxView}, {"componentType", 5125},
            {"count", texIdxCount}, {"type", "SCALAR"}
        });
    }
    gltf["accessors"] = accessors;

    // Image / Sampler / Texture (only if textured prim is present).
    if (hasTex) {
        gltf["images"]   = json::array({ {{"bufferView", atlasImgView},
                                          {"mimeType", "image/png"}} });
        // NEAREST + CLAMP_TO_EDGE so the baked atlas reads cell-perfectly and
        // sampling at patch borders doesn't bleed across into adjacent ones.
        gltf["samplers"] = json::array({ {
            {"magFilter", 9728}, // NEAREST
            {"minFilter", 9728}, // NEAREST
            {"wrapS", 33071},    // CLAMP_TO_EDGE
            {"wrapT", 33071}
        } });
        gltf["textures"] = json::array({ {{"source", 0}, {"sampler", 0}} });
    }

    // Primitives.
    json primitives = json::array();
    if (hasVc) {
        json p;
        p["attributes"] = {
            {"POSITION", vcPosAcc},
            {"NORMAL",   vcNormAcc},
            {"COLOR_0",  vcColorAcc}
        };
        p["indices"]  = vcIdxAcc;
        p["material"] = vcMatIndex;
        primitives.push_back(p);
    }
    if (hasTex) {
        json p;
        p["attributes"] = {
            {"POSITION",   texPosAcc},
            {"NORMAL",     texNormAcc},
            {"TEXCOORD_0", texUvAcc}
        };
        p["indices"]  = texIdxAcc;
        p["material"] = texMatIndex;
        primitives.push_back(p);
    }
    gltf["meshes"] = json::array({ {{"primitives", primitives}} });

    gltf["buffers"] = json::array({ {{"byteLength", binBuffer.size()}} });

    // Serialize and pad JSON to 4 bytes
    std::string jsonStr = gltf.dump();
    while (jsonStr.size() % 4 != 0) jsonStr.push_back(' ');

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "[gltf] Failed to open: " << outPath << std::endl;
        return false;
    }

    uint32_t jsonChunkLen = (uint32_t)jsonStr.size();
    uint32_t binChunkLen  = (uint32_t)binBuffer.size();
    uint32_t totalLen     = 12 + 8 + jsonChunkLen + 8 + binChunkLen;

    uint32_t magic   = 0x46546C67; // "glTF"
    uint32_t version = 2;
    out.write(reinterpret_cast<const char*>(&magic),    4);
    out.write(reinterpret_cast<const char*>(&version),  4);
    out.write(reinterpret_cast<const char*>(&totalLen), 4);

    uint32_t jsonType = 0x4E4F534A; // "JSON"
    out.write(reinterpret_cast<const char*>(&jsonChunkLen), 4);
    out.write(reinterpret_cast<const char*>(&jsonType),     4);
    out.write(jsonStr.data(), jsonChunkLen);

    uint32_t binType = 0x004E4942; // "BIN\0"
    out.write(reinterpret_cast<const char*>(&binChunkLen), 4);
    out.write(reinterpret_cast<const char*>(&binType),     4);
    out.write(reinterpret_cast<const char*>(binBuffer.data()), binChunkLen);

    std::cout << "[gltf] Exported " << emittedTris << " tris ("
              << greedyQuads << " vc-merged, "
              << bakedQuads << " textured, "
              << culledTris << " culled), "
              << (vcVertCount + texVertCount) << " verts";
    if (hasTex) {
        std::cout << ", atlas " << atlas.w << "x" << atlas.h
                  << " (" << atlasPNG.size() << "B)";
    }
    std::cout << " -> " << outPath << std::endl;
    return true;
}
