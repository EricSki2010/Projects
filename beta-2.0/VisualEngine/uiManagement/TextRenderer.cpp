#include "TextRenderer.h"
#include "TextShaders.h"
#include "../renderingManagement/render.h"
#include "../EngineGlobals.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <memory>
#include <unordered_map>
#include <iostream>

struct GlyphInfo {
    unsigned int textureId;
    glm::ivec2 size;      // width, height in pixels
    glm::ivec2 bearing;   // offset from baseline
    int advance;           // horizontal advance in 1/64 pixels
};

static std::unordered_map<char, GlyphInfo> sGlyphs;
static unsigned int sTextVAO = 0;
static unsigned int sTextVBO = 0;
static std::unique_ptr<Shader> sTextShader;

static bool loadGlyphs(FT_Face face, int fontSize) {
    FT_Set_Pixel_Sizes(face, 0, fontSize);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    for (unsigned char c = 0; c < 128; c++) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER)) continue;

        unsigned int tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
                     face->glyph->bitmap.width, face->glyph->bitmap.rows,
                     0, GL_RED, GL_UNSIGNED_BYTE, face->glyph->bitmap.buffer);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GlyphInfo glyph;
        glyph.textureId = tex;
        glyph.size = glm::ivec2(face->glyph->bitmap.width, face->glyph->bitmap.rows);
        glyph.bearing = glm::ivec2(face->glyph->bitmap_left, face->glyph->bitmap_top);
        glyph.advance = (int)face->glyph->advance.x;
        sGlyphs[c] = glyph;
    }
    return true;
}

static void initTextGL() {
    sTextShader = std::make_unique<Shader>(textVertSrc, textFragSrc);

    glGenVertexArrays(1, &sTextVAO);
    glGenBuffers(1, &sTextVBO);
    glBindVertexArray(sTextVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sTextVBO);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

bool initTextRenderer(const char* fontPath, int fontSize) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        std::cerr << "FreeType: failed to init library" << std::endl;
        return false;
    }

    FT_Face face;
    if (FT_New_Face(ft, fontPath, 0, &face)) {
        std::cerr << "FreeType: failed to load font: " << fontPath << std::endl;
        FT_Done_FreeType(ft);
        return false;
    }

    loadGlyphs(face, fontSize);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    initTextGL();
    return true;
}

bool initTextRendererFromMemory(const unsigned char* data, unsigned int dataSize, int fontSize) {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        std::cerr << "FreeType: failed to init library" << std::endl;
        return false;
    }

    FT_Face face;
    if (FT_New_Memory_Face(ft, data, dataSize, 0, &face)) {
        std::cerr << "FreeType: failed to load font from memory" << std::endl;
        FT_Done_FreeType(ft);
        return false;
    }

    loadGlyphs(face, fontSize);
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    initTextGL();
    return true;
}

void cleanupTextRenderer() {
    for (auto& [c, g] : sGlyphs)
        glDeleteTextures(1, &g.textureId);
    sGlyphs.clear();

    if (sTextVAO) { glDeleteVertexArrays(1, &sTextVAO); sTextVAO = 0; }
    if (sTextVBO) { glDeleteBuffers(1, &sTextVBO); sTextVBO = 0; }
    sTextShader.reset();
}

void drawText(const std::string& text, float x, float y, float scale, const glm::vec4& color) {
    if (!sTextShader) return;

    sTextShader->use();
    glUniform4f(sTextShader->loc("uColor"), color.r, color.g, color.b, color.a);
    glUniform2f(sTextShader->loc("uScreenSize"), (float)ctx.width, (float)ctx.height);
    glUniform1i(sTextShader->loc("uGlyph"), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(sTextVAO);

    float cursorX = x;
    for (char c : text) {
        auto it = sGlyphs.find(c);
        if (it == sGlyphs.end()) continue;
        const GlyphInfo& g = it->second;

        float xpos = cursorX + g.bearing.x * scale;
        float ypos = y + (sGlyphs['H'].bearing.y - g.bearing.y) * scale;
        float w = g.size.x * scale;
        float h = g.size.y * scale;

        float vertices[6][4] = {
            { xpos,     ypos,       0.0f, 0.0f },
            { xpos + w, ypos,       1.0f, 0.0f },
            { xpos + w, ypos + h,   1.0f, 1.0f },
            { xpos,     ypos,       0.0f, 0.0f },
            { xpos + w, ypos + h,   1.0f, 1.0f },
            { xpos,     ypos + h,   0.0f, 1.0f },
        };

        glBindTexture(GL_TEXTURE_2D, g.textureId);
        glBindBuffer(GL_ARRAY_BUFFER, sTextVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        cursorX += (g.advance >> 6) * scale;
    }

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

float measureText(const std::string& text, float scale) {
    float width = 0;
    for (char c : text) {
        auto it = sGlyphs.find(c);
        if (it == sGlyphs.end()) continue;
        width += (it->second.advance >> 6) * scale;
    }
    return width;
}

float measureTextHeight(float scale) {
    auto it = sGlyphs.find('H');
    if (it == sGlyphs.end()) return 0;
    return it->second.bearing.y * scale;
}
