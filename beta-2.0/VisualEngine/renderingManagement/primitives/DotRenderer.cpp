#include "DotRenderer.h"
#include "DotShaders.h"
#include "../render.h"
#include "../../EngineGlobals.h"
#include <memory>

static unsigned int sDotVAO = 0;
static unsigned int sDotVBO = 0;
static unsigned int sDotEBO = 0;
static std::unique_ptr<Shader> sDotShader;

void initDotRenderer() {
    sDotShader = std::make_unique<Shader>(dotVertSrc, dotFragSrc);

    // Unit quad centered at origin
    float verts[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
         0.5f,  0.5f,
        -0.5f,  0.5f,
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

    glGenVertexArrays(1, &sDotVAO);
    glGenBuffers(1, &sDotVBO);
    glGenBuffers(1, &sDotEBO);

    glBindVertexArray(sDotVAO);

    glBindBuffer(GL_ARRAY_BUFFER, sDotVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sDotEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void cleanupDotRenderer() {
    if (sDotVAO) { glDeleteVertexArrays(1, &sDotVAO); sDotVAO = 0; }
    if (sDotVBO) { glDeleteBuffers(1, &sDotVBO); sDotVBO = 0; }
    if (sDotEBO) { glDeleteBuffers(1, &sDotEBO); sDotEBO = 0; }
    sDotShader.reset();
}

void drawDot(const glm::vec3& position, float size, const glm::vec3& color, float alpha) {
    if (!sDotShader) return;

    sDotShader->use();
    glUniform3fv(sDotShader->loc("uCenter"), 1, glm::value_ptr(position));

    // Scale size by distance from camera
    glm::vec3 camPos = glm::vec3(glm::inverse(ctx.scene->view)[3]);
    float dist = glm::length(position - camPos);
    float scaledSize = size * 2.0f * glm::clamp(1.0f / (dist * 0.5f + 1.0f), 0.1f, 1.0f);
    scaledSize = glm::min(scaledSize, size * 2.0f);
    glUniform1f(sDotShader->loc("uSize"), scaledSize);
    glUniform3fv(sDotShader->loc("uColor"), 1, glm::value_ptr(color));
    glUniform1f(sDotShader->loc("uAlpha"), alpha);
    glUniformMatrix4fv(sDotShader->loc("view"), 1, GL_FALSE, glm::value_ptr(ctx.scene->view));
    glUniformMatrix4fv(sDotShader->loc("projection"), 1, GL_FALSE, glm::value_ptr(ctx.scene->projection));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthFunc(GL_LEQUAL);
    glBindVertexArray(sDotVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);

    glBindVertexArray(0);
}
