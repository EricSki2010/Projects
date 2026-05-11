#pragma once

// ── Greedy meshing utility (header-only) ───────────────────────────────
//
// Color-aware 2D greedy meshing for axis-aligned cube faces. Adapted from
// the modelEditor's GltfExporter; factored out so both the GLB exporter
// and the runtime chunk renderer can share the algorithmic core.
//
// Caller responsibilities:
//   1. Produce a list of GMCell{u, v, paletteIndex} per (faceDir,
//      planeIntCoord) plane, including culling decisions.
//   2. Pass an `emit` callback that converts each merged rectangle into
//      whatever vertex/index format the caller needs (interleaved buffer,
//      glTF primitive, etc).
//
// The algorithm itself is a standard "extend width on row, then extend
// height while every row in the strip matches" sweep — O(N) over the
// plane's cells in the merge case, with worst-case O(W*H*W) for highly
// fragmented planes. Good enough for chunk-sized inputs.

#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <climits>

namespace VEGreedy {

// (normalAxis, uAxis, vAxis) for each face direction in cardinal order:
// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z. uAxis < vAxis by convention.
struct FaceAxes { int normalAxis, uAxis, vAxis; };

inline const FaceAxes kFaceAxes[6] = {
    {0, 1, 2}, {0, 1, 2},
    {1, 0, 2}, {1, 0, 2},
    {2, 0, 1}, {2, 0, 1},
};

inline const glm::vec3 kCardinalDirs[6] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1},
};

inline bool isPositiveFace(int faceDir) {
    return faceDir == 0 || faceDir == 2 || faceDir == 4;
}

// Plane identity: (faceDir, planeIntCoord). planeIntCoord packs the
// world-space half-integer (cube AABB face plane is at integer ± 0.5)
// into a regular int: planeIntCoord = 2*pos_along_normal + sign.
inline int planeIntCoordFor(int faceDir, const glm::ivec3& pos) {
    int posAlong = pos[kFaceAxes[faceDir].normalAxis];
    return 2 * posAlong + (isPositiveFace(faceDir) ? 1 : -1);
}

struct GMCell {
    int u, v;
    int paletteIndex; // -1 = unpainted; equal indices merge together
};

// 64-bit hash key for (u, v) cell coords, handles negatives via two's
// complement. Used to bucket cells in the per-plane grid.
inline int64_t cellKey(int u, int v) {
    return ((int64_t)(uint32_t)u << 32) | (uint32_t)v;
}

// Run color-aware 2D greedy meshing on one plane's worth of cells. The
// emit callback receives the merged rectangle (inclusive bounds) and the
// shared paletteIndex. Returns the number of merged quads emitted.
//
// emit signature:
//   void(int faceDir, int planeIntCoord,
//        int uMin, int uMax, int vMin, int vMax,
//        int paletteIndex)
template<typename EmitFn>
inline int greedyMeshPlane(int faceDir, int planeIntCoord,
                           const std::vector<GMCell>& cells, EmitFn&& emit) {
    if (cells.empty()) return 0;

    std::unordered_map<int64_t, int> grid;
    int uMin = INT_MAX, uMax = INT_MIN, vMin = INT_MAX, vMax = INT_MIN;
    grid.reserve(cells.size() * 2);
    for (const auto& c : cells) {
        grid[cellKey(c.u, c.v)] = c.paletteIndex;
        if (c.u < uMin) uMin = c.u;
        if (c.u > uMax) uMax = c.u;
        if (c.v < vMin) vMin = c.v;
        if (c.v > vMax) vMax = c.v;
    }

    std::unordered_set<int64_t> processed;
    processed.reserve(cells.size() * 2);
    int quadsEmitted = 0;

    for (int v = vMin; v <= vMax; v++) {
        for (int u = uMin; u <= uMax; u++) {
            int64_t k = cellKey(u, v);
            if (processed.count(k)) continue;
            auto it = grid.find(k);
            if (it == grid.end()) continue;
            int paletteIndex = it->second;

            // Extend width along this row while cells stay same-color and
            // unprocessed.
            int wMax = 0;
            for (int w = 0; u + w <= uMax; w++) {
                int64_t kw = cellKey(u + w, v);
                if (processed.count(kw)) break;
                auto it2 = grid.find(kw);
                if (it2 == grid.end() || it2->second != paletteIndex) break;
                wMax = w + 1;
            }

            // Extend height: every cell in the wMax-wide strip must match.
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

            emit(faceDir, planeIntCoord,
                 u, u + wMax - 1, v, v + hMax - 1,
                 paletteIndex);
            quadsEmitted++;
        }
    }
    return quadsEmitted;
}

} // namespace VEGreedy
