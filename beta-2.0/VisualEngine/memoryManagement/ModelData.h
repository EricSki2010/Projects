#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>

struct FaceColor {
    glm::vec3 color = glm::vec3(0.8f); // default grey
};

struct BlockTypeDef {
    std::string name;
    std::vector<float> vertices;     // pos3+uv2 (5) or pos3+uv2+normal3 (8) per vertex
    int vertexCount = 0;
    int floatsPerVertex = 5;
    std::vector<unsigned int> indices;
    int indexCount = 0;
    std::vector<FaceColor> faceColors; // one per triangle (indexCount / 3)
    std::string texturePath;           // optional image path applied via UVs; empty = none
};

struct BlockPlacement {
    int x, y, z;
    int typeId;
    int rx, ry, rz;
    std::vector<int16_t> triColors; // global palette index per triangle (pack*16+slot, -1 = unpainted)
};

struct ModelFile {
    std::vector<BlockTypeDef> blockTypes;
    std::vector<BlockPlacement> placements;
    // Variable-size palette; size is always a multiple of 16 (one pack = 16 colors).
    // Default: one pack of 16 grey entries.
    std::vector<glm::vec3> palette = std::vector<glm::vec3>(16, glm::vec3(0.6f, 0.6f, 0.6f));
};
