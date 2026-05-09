#include "LineRenderer.h"
#include "LineShaders.h"
#include "../render.h"
#include "../../EngineGlobals.h"
#include <memory>

static unsigned int sLineVAO = 0;
static unsigned int sLineVBO = 0;
static std::unique_ptr<Shader> sLineShader;
static const int MAX_LINE_VERTS = 4096;

void initLineRenderer() {
    sLineShader = std::make_unique<Shader>(lineVertSrc, lineFragSrc);

    glGenVertexArrays(1, &sLineVAO);
    glGenBuffers(1, &sLineVBO);

    glBindVertexArray(sLineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sLineVBO);
    glBufferData(GL_ARRAY_BUFFER, MAX_LINE_VERTS * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void cleanupLineRenderer() {
    if (sLineVAO) { glDeleteVertexArrays(1, &sLineVAO); sLineVAO = 0; }
    if (sLineVBO) { glDeleteBuffers(1, &sLineVBO); sLineVBO = 0; }
    sLineShader.reset();
}

void drawLine(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color, float width) {
    if (!sLineShader) return;

    float verts[] = { from.x, from.y, from.z, to.x, to.y, to.z };

    sLineShader->use();
    glUniformMatrix4fv(sLineShader->loc("view"), 1, GL_FALSE, glm::value_ptr(ctx.scene->view));
    glUniformMatrix4fv(sLineShader->loc("projection"), 1, GL_FALSE, glm::value_ptr(ctx.scene->projection));
    glUniform3f(sLineShader->loc("uColor"), color.r, color.g, color.b);

    glBindVertexArray(sLineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sLineVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glLineWidth(width);
    glDrawArrays(GL_LINES, 0, 2);

    glBindVertexArray(0);
}

void drawLines(const std::vector<glm::vec3>& points, const glm::vec3& color, float width, bool loop) {
    if (!sLineShader || points.size() < 2) return;

    int count = (int)points.size();
    if (count > MAX_LINE_VERTS) count = MAX_LINE_VERTS;

    sLineShader->use();
    glUniformMatrix4fv(sLineShader->loc("view"), 1, GL_FALSE, glm::value_ptr(ctx.scene->view));
    glUniformMatrix4fv(sLineShader->loc("projection"), 1, GL_FALSE, glm::value_ptr(ctx.scene->projection));
    glUniform3f(sLineShader->loc("uColor"), color.r, color.g, color.b);

    glBindVertexArray(sLineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sLineVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, count * 3 * sizeof(float), &points[0]);

    glLineWidth(width);
    glDrawArrays(loop ? GL_LINE_LOOP : GL_LINE_STRIP, 0, count);

    glBindVertexArray(0);
}
