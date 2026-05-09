#include "memory.h"
#include "ModelData.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

static std::string sMemoryPath;

void setMemoryPath(const std::string& dir) {
    sMemoryPath = dir;
}

// Parse a format file into ordered field names and bit widths
static bool parseFormat(const std::string& formatPath,
                        std::vector<std::string>& fieldNames,
                        std::vector<int>& bitWidths) {
    std::ifstream file(formatPath);
    if (!file) {
        std::cerr << "saveToMemory: format file not found: " << formatPath << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        size_t arrow = line.find("->");
        if (arrow == std::string::npos) continue;

        std::string name = line.substr(0, arrow);
        std::string bits = line.substr(arrow + 2);

        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!bits.empty() && bits.front() == ' ') bits.erase(bits.begin());

        fieldNames.push_back(name);
        bitWidths.push_back(std::stoi(bits));
    }

    return !fieldNames.empty();
}

static int totalBits(const std::vector<int>& bitWidths) {
    int total = 0;
    for (int w : bitWidths) total += w;
    return total;
}

// Parse comma-separated values from a string
static std::vector<uint64_t> parseValues(const std::string& entry) {
    std::vector<uint64_t> values;
    std::stringstream ss(entry);
    std::string token;
    while (std::getline(ss, token, ',')) {
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        values.push_back((uint64_t)std::stoull(token));
    }
    return values;
}

// Pack a single entry's fields into one uint64_t
static uint64_t packEntry(const std::vector<uint64_t>& values,
                          const std::vector<int>& bitWidths) {
    uint64_t packed = 0;
    int shift = 0;
    for (int i = (int)values.size() - 1; i >= 0; i--) {
        packed |= (values[i] & ((1ULL << bitWidths[i]) - 1)) << shift;
        shift += bitWidths[i];
    }
    return packed;
}

// Unpack a uint64_t back into field values
static std::vector<uint64_t> unpackEntry(uint64_t packed,
                                         const std::vector<int>& bitWidths) {
    std::vector<uint64_t> values(bitWidths.size());
    int shift = 0;
    for (int i = (int)bitWidths.size() - 1; i >= 0; i--) {
        values[i] = (packed >> shift) & ((1ULL << bitWidths[i]) - 1);
        shift += bitWidths[i];
    }
    return values;
}

// RLE encode: consecutive identical packed values become [value, count] pairs
static std::vector<uint64_t> rleEncode(const std::vector<uint64_t>& packed) {
    std::vector<uint64_t> encoded;
    if (packed.empty()) return encoded;

    uint64_t current = packed[0];
    uint64_t count = 1;
    for (size_t i = 1; i < packed.size(); i++) {
        if (packed[i] == current && count < 0xFFFF) {
            count++;
        } else {
            encoded.push_back(current);
            encoded.push_back(count);
            current = packed[i];
            count = 1;
        }
    }
    encoded.push_back(current);
    encoded.push_back(count);
    return encoded;
}

// RLE decode: expand [value, count] pairs back into flat array
static std::vector<uint64_t> rleDecode(const std::vector<uint64_t>& encoded) {
    std::vector<uint64_t> packed;
    for (size_t i = 0; i + 1 < encoded.size(); i += 2) {
        uint64_t value = encoded[i];
        uint64_t count = encoded[i + 1];
        for (uint64_t c = 0; c < count; c++)
            packed.push_back(value);
    }
    return packed;
}

bool saveToMemory(const std::string& name, const std::string& formatPath,
                  const std::vector<std::string>& data) {
    std::vector<std::string> fieldNames;
    std::vector<int> bitWidths;
    if (!parseFormat(formatPath, fieldNames, bitWidths))
        return false;

    // Determine 32 or 64 bit mode
    bool use64 = totalBits(bitWidths) > 32;
    uint8_t mode = use64 ? 64 : 32;
    int unitSize = use64 ? 8 : 4;

    // Pack each entry
    std::vector<uint64_t> packed;
    for (const auto& entry : data) {
        std::vector<uint64_t> values = parseValues(entry);
        if (values.size() != bitWidths.size()) {
            std::cerr << "saveToMemory: entry has " << values.size()
                      << " values, format expects " << bitWidths.size() << std::endl;
            return false;
        }
        packed.push_back(packEntry(values, bitWidths));
    }

    // RLE compress
    std::vector<uint64_t> encoded = rleEncode(packed);

    // Write binary file: [mode:8][entryCount:32][rleSize:32][rle data...]
    std::string path = sMemoryPath + "/" + name + ".bin";
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "saveToMemory: failed to write: " << path << std::endl;
        return false;
    }

    uint32_t entryCount = (uint32_t)data.size();
    uint32_t rleSize = (uint32_t)encoded.size();
    out.write(reinterpret_cast<const char*>(&mode), 1);
    out.write(reinterpret_cast<const char*>(&entryCount), 4);
    out.write(reinterpret_cast<const char*>(&rleSize), 4);

    if (use64) {
        out.write(reinterpret_cast<const char*>(encoded.data()), rleSize * 8);
    } else {
        // Write as 32-bit to save space
        for (uint64_t v : encoded) {
            uint32_t v32 = (uint32_t)v;
            out.write(reinterpret_cast<const char*>(&v32), 4);
        }
    }
    return true;
}

std::vector<std::string> loadFromMemory(const std::string& name, const std::string& formatPath) {
    std::vector<std::string> fieldNames;
    std::vector<int> bitWidths;
    if (!parseFormat(formatPath, fieldNames, bitWidths))
        return {};

    std::string path = sMemoryPath + "/" + name + ".bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "loadFromMemory: file not found: " << path << std::endl;
        return {};
    }

    // Read header
    uint8_t mode = 0;
    uint32_t entryCount = 0, rleSize = 0;
    in.read(reinterpret_cast<char*>(&mode), 1);
    in.read(reinterpret_cast<char*>(&entryCount), 4);
    in.read(reinterpret_cast<char*>(&rleSize), 4);

    bool use64 = (mode == 64);

    // Read RLE data
    std::vector<uint64_t> encoded(rleSize);
    if (use64) {
        in.read(reinterpret_cast<char*>(encoded.data()), rleSize * 8);
    } else {
        for (uint32_t i = 0; i < rleSize; i++) {
            uint32_t v32 = 0;
            in.read(reinterpret_cast<char*>(&v32), 4);
            encoded[i] = v32;
        }
    }

    // RLE decompress
    std::vector<uint64_t> packed = rleDecode(encoded);

    // Unpack each entry
    std::vector<std::string> result;
    for (uint32_t e = 0; e < entryCount && e < packed.size(); e++) {
        std::vector<uint64_t> values = unpackEntry(packed[e], bitWidths);
        std::string entry;
        for (size_t i = 0; i < values.size(); i++) {
            if (i > 0) entry += ",";
            entry += std::to_string(values[i]);
        }
        result.push_back(entry);
    }

    return result;
}

// ── Model save/load ─────────────────────────────────────────────────

static void writeU32(std::ofstream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}

static void writeF32(std::ofstream& out, float v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}

static uint32_t readU32(std::ifstream& in) {
    uint32_t v = 0;
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}

static float readF32(std::ifstream& in) {
    float v = 0;
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}

static void writeString(std::ofstream& out, const std::string& s) {
    writeU32(out, (uint32_t)s.size());
    out.write(s.data(), s.size());
}

static std::string readString(std::ifstream& in) {
    uint32_t len = readU32(in);
    if (len > 4096) return "";
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

bool saveModel(const std::string& name, const ModelFile& model) {
    std::string path = sMemoryPath + "/" + name + ".mdl";
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "saveModel: failed to write: " << path << std::endl;
        return false;
    }

    // Magic + version
    // v5: per-block-type texturePath string, written after indices and before
    //     the per-triangle face colors.
    // v4: triColors are int16_t (was int8_t in v3), palette is variable-size
    //     (u32 count + count*vec3) so multi-pack paint palettes can persist.
    out.write("MDL", 3);
    uint8_t version = 5;
    out.write(reinterpret_cast<const char*>(&version), 1);

    // Block types section
    writeU32(out, (uint32_t)model.blockTypes.size());
    for (const auto& bt : model.blockTypes) {
        writeString(out, bt.name);
        writeU32(out, (uint32_t)bt.vertexCount);
        writeU32(out, (uint32_t)bt.indexCount);
        writeU32(out, (uint32_t)bt.floatsPerVertex);

        // Vertices
        for (float v : bt.vertices) writeF32(out, v);

        // Indices
        for (unsigned int idx : bt.indices) writeU32(out, idx);

        // Texture path (v5+; empty string = no texture)
        writeString(out, bt.texturePath);

        // Face colors (one per triangle)
        uint32_t triCount = bt.indexCount / 3;
        writeU32(out, triCount);
        for (uint32_t i = 0; i < triCount; i++) {
            if (i < bt.faceColors.size()) {
                writeF32(out, bt.faceColors[i].color.r);
                writeF32(out, bt.faceColors[i].color.g);
                writeF32(out, bt.faceColors[i].color.b);
            } else {
                writeF32(out, 0.8f);
                writeF32(out, 0.8f);
                writeF32(out, 0.8f);
            }
        }
    }

    // Placements section
    writeU32(out, (uint32_t)model.placements.size());
    for (const auto& p : model.placements) {
        writeU32(out, (uint32_t)p.x);
        writeU32(out, (uint32_t)p.y);
        writeU32(out, (uint32_t)p.z);
        writeU32(out, (uint32_t)p.typeId);
        writeU32(out, (uint32_t)p.rx);
        writeU32(out, (uint32_t)p.ry);
        writeU32(out, (uint32_t)p.rz);
        writeU32(out, (uint32_t)p.triColors.size());
        for (int16_t c : p.triColors)
            out.write(reinterpret_cast<const char*>(&c), 2);
    }

    // Palette (v4: variable size, multiple of 16)
    uint32_t paletteCount = (uint32_t)model.palette.size();
    if (paletteCount == 0) paletteCount = 16;
    writeU32(out, paletteCount);
    for (uint32_t i = 0; i < paletteCount; i++) {
        glm::vec3 c = (i < model.palette.size()) ? model.palette[i] : glm::vec3(0.6f);
        writeF32(out, c.r);
        writeF32(out, c.g);
        writeF32(out, c.b);
    }

    return true;
}

bool loadModel(const std::string& name, ModelFile& model) {
    std::string path = sMemoryPath + "/" + name + ".mdl";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "loadModel: file not found: " << path << std::endl;
        return false;
    }

    // Magic + version
    char magic[3];
    in.read(magic, 3);
    if (magic[0] != 'M' || magic[1] != 'D' || magic[2] != 'L') {
        std::cerr << "loadModel: invalid file format: " << path << std::endl;
        return false;
    }
    uint8_t version = 0;
    in.read(reinterpret_cast<char*>(&version), 1);

    // Block types section
    uint32_t typeCount = readU32(in);
    model.blockTypes.resize(typeCount);
    for (uint32_t t = 0; t < typeCount; t++) {
        BlockTypeDef& bt = model.blockTypes[t];
        bt.name = readString(in);
        bt.vertexCount = (int)readU32(in);
        bt.indexCount = (int)readU32(in);
        bt.floatsPerVertex = (version >= 2) ? (int)readU32(in) : 5;

        int totalFloats = bt.vertexCount * bt.floatsPerVertex;
        bt.vertices.resize(totalFloats);
        for (int i = 0; i < totalFloats; i++)
            bt.vertices[i] = readF32(in);

        bt.indices.resize(bt.indexCount);
        for (int i = 0; i < bt.indexCount; i++)
            bt.indices[i] = readU32(in);

        if (version >= 5) {
            bt.texturePath = readString(in);
        } else {
            bt.texturePath.clear();
        }

        uint32_t triCount = readU32(in);
        bt.faceColors.resize(triCount);
        for (uint32_t i = 0; i < triCount; i++) {
            bt.faceColors[i].color.r = readF32(in);
            bt.faceColors[i].color.g = readF32(in);
            bt.faceColors[i].color.b = readF32(in);
        }
    }

    // Placements section
    uint32_t placementCount = readU32(in);
    model.placements.resize(placementCount);
    for (uint32_t i = 0; i < placementCount; i++) {
        BlockPlacement& p = model.placements[i];
        p.x = (int)readU32(in);
        p.y = (int)readU32(in);
        p.z = (int)readU32(in);
        p.typeId = (int)readU32(in);
        p.rx = (int)readU32(in);
        p.ry = (int)readU32(in);
        p.rz = (int)readU32(in);
        if (version >= 4) {
            uint32_t triColorCount = readU32(in);
            p.triColors.resize(triColorCount);
            for (uint32_t j = 0; j < triColorCount; j++) {
                int16_t v = 0;
                in.read(reinterpret_cast<char*>(&v), 2);
                p.triColors[j] = v;
            }
        } else if (version >= 3) {
            // v3 wrote int8_t; sign-extend into the new int16_t storage.
            uint32_t triColorCount = readU32(in);
            p.triColors.resize(triColorCount);
            for (uint32_t j = 0; j < triColorCount; j++) {
                int8_t v = 0;
                in.read(reinterpret_cast<char*>(&v), 1);
                p.triColors[j] = (int16_t)v;
            }
        }
    }

    // Palette
    if (version >= 4) {
        uint32_t paletteCount = readU32(in);
        if (paletteCount == 0) paletteCount = 16;
        model.palette.resize(paletteCount);
        for (uint32_t i = 0; i < paletteCount; i++) {
            model.palette[i].r = readF32(in);
            model.palette[i].g = readF32(in);
            model.palette[i].b = readF32(in);
        }
    } else if (version >= 3) {
        // v3 had a fixed 16-entry palette.
        model.palette.resize(16);
        for (int i = 0; i < 16; i++) {
            model.palette[i].r = readF32(in);
            model.palette[i].g = readF32(in);
            model.palette[i].b = readF32(in);
        }
    }

    return true;
}
