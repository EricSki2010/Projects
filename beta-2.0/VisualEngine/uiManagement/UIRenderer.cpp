#include "UIRenderer.h"
#include "UIShaders.h"
#include "../renderingManagement/render.h"
#include "../EngineGlobals.h"
#include <memory>

static unsigned int sUIVAO = 0;
static unsigned int sUIVBO = 0;
static unsigned int sUIEBO = 0;
static std::unique_ptr<Shader> sUIShader;

void initUIRenderer() {
    sUIShader = std::make_unique<Shader>(uiVertSrc, uiFragSrc);

    // Unit quad: bottom-left at (0,0), top-right at (1,1)
    float vertices[] = {
        // pos       // uv
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

    glGenVertexArrays(1, &sUIVAO);
    glGenBuffers(1, &sUIVBO);
    glGenBuffers(1, &sUIEBO);

    glBindVertexArray(sUIVAO);

    glBindBuffer(GL_ARRAY_BUFFER, sUIVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sUIEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void cleanupUIRenderer() {
    if (sUIVAO) { glDeleteVertexArrays(1, &sUIVAO); sUIVAO = 0; }
    if (sUIVBO) { glDeleteBuffers(1, &sUIVBO); sUIVBO = 0; }
    if (sUIEBO) { glDeleteBuffers(1, &sUIEBO); sUIEBO = 0; }
    sUIShader.reset();
}

void drawUIElement(const UIElement& element) {
    if (!element.visible || !sUIShader) return;

    sUIShader->use();

    float sizeX = element.size.x;
    if (element.aspectCorrected && ctx.width > 0) {
        float aspect = (float)ctx.width / (float)ctx.height;
        sizeX = element.size.x / aspect;
    }
    glUniform2f(sUIShader->loc("uPosition"), element.position.x, element.position.y);
    glUniform2f(sUIShader->loc("uSize"), sizeX, element.size.y);
    glUniform4f(sUIShader->loc("uColor"), element.color.r, element.color.g, element.color.b, element.color.a);

    // Pixel-space dimensions (NDC span is 2.0 across the screen) drive the rounded-corner SDF.
    float pixW = sizeX * 0.5f * (float)ctx.width;
    float pixH = element.size.y * 0.5f * (float)ctx.height;
    glUniform2f(sUIShader->loc("uPixelSize"), pixW, pixH);
    glUniform1f(sUIShader->loc("uCornerRadius"), element.cornerRadius);

    bool hasTexture = element.textureId != 0;
    glUniform1i(sUIShader->loc("uUseTexture"), hasTexture ? 1 : 0);
    if (hasTexture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, element.textureId);
        glUniform1i(sUIShader->loc("uTexture"), 0);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(sUIVAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

Shader* getUIShader() {
    return sUIShader.get();
}
