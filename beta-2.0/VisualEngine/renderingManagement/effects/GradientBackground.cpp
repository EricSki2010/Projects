#include "GradientBackground.h"
#include "GradientShaders.h"
#include "../render.h"
#include "../../EngineGlobals.h"
#include <memory>

static unsigned int sGradVAO = 0;
static unsigned int sGradVBO = 0;
static std::unique_ptr<Shader> sGradShader;
static bool sEnabled = false;
static glm::vec3 sTopColor = glm::vec3(0.0f);
static glm::vec3 sBottomColor = glm::vec3(0.7f);

void initGradientBackground() {
    sGradShader = std::make_unique<Shader>(gradientVertSrc, gradientFragSrc);

    // Fullscreen quad
    float verts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };

    glGenVertexArrays(1, &sGradVAO);
    glGenBuffers(1, &sGradVBO);
    glBindVertexArray(sGradVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sGradVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void cleanupGradientBackground() {
    if (sGradVAO) { glDeleteVertexArrays(1, &sGradVAO); sGradVAO = 0; }
    if (sGradVBO) { glDeleteBuffers(1, &sGradVBO); sGradVBO = 0; }
    sGradShader.reset();
}

void setGradientColors(const glm::vec3& top, const glm::vec3& bottom) {
    sTopColor = top;
    sBottomColor = bottom;
}

void enableGradientBackground(bool enable) {
    sEnabled = enable;
}

bool isGradientBackgroundEnabled() {
    return sEnabled;
}

void drawGradientBackground() {
    if (!sEnabled || !sGradShader) return;

    sGradShader->use();

    // Inverse view-projection to reconstruct world direction from screen coords
    glm::mat4 invViewProj = glm::inverse(ctx.scene->projection * ctx.scene->view);
    glUniformMatrix4fv(sGradShader->loc("uInvViewProj"), 1, GL_FALSE, glm::value_ptr(invViewProj));
    glUniform3fv(sGradShader->loc("uTopColor"), 1, glm::value_ptr(sTopColor));
    glUniform3fv(sGradShader->loc("uBottomColor"), 1, glm::value_ptr(sBottomColor));

    glDepthMask(GL_FALSE); // Don't write to depth buffer
    glBindVertexArray(sGradVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDepthMask(GL_TRUE);

    glBindVertexArray(0);
}
