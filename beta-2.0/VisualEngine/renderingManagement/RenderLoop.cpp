#include "render.h"
#include "meshing/ChunkMesh.h"
#include "effects/GradientBackground.h"
#include "../EngineGlobals.h"
#include "../sceneManagement/SceneManager.h"
#include "../inputManagement/Camera.h"

namespace VE { void pollSceneCycleHotkey(); }

void processInput(float dt) {
    getGlobalCamera()->processKeyboard(ctx.window, dt);

    VE::pollSceneCycleHotkey();

    SceneDef* scene = getActiveScene();
    if (scene && scene->onInput)
        scene->onInput(dt);

    ctx.scrollDelta = 0.0f;
}

void update() {
    if (ctx.needsRebuild)
        VE::rebuild();

    ctx.scene->view = getGlobalCamera()->getViewMatrix();

    SceneDef* scene = getActiveScene();
    if (scene && scene->onUpdate)
        scene->onUpdate();
}

void render() {
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    drawGradientBackground();

    ctx.shader->use();
    glUniform1f(ctx.shader->loc("brightness"), 1.0f); // reset brightness each frame
    Camera* cam = getGlobalCamera();
    glUniformMatrix4fv(ctx.shader->loc("view"), 1, GL_FALSE, glm::value_ptr(ctx.scene->view));
    glUniform3fv(ctx.shader->loc("viewPos"), 1, glm::value_ptr(cam->position));
    glm::vec3 lightPos = cam->position + glm::vec3(0.0f, 2.0f, 0.0f);
    glUniform3fv(ctx.shader->loc("lightPos"), 1, glm::value_ptr(lightPos));

    glm::mat4 model(1.0f);
    ctx.scene->uploadFrameUniforms(*ctx.shader, model);
    const Fog& fog = ctx.scene->fog;
    const glm::vec3 camPos = cam->position;

    // Frustum-plane extraction (Gribb-Hartmann) from projection*view. Each
    // plane is normalized so the signed distance from a point to the plane
    // is `dot(normal, point) + d`. A sphere is fully outside a plane when
    // that distance < -radius.
    glm::mat4 vp = ctx.scene->projection * ctx.scene->view;
    glm::vec4 row0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    glm::vec4 row1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    glm::vec4 row2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    glm::vec4 row3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    glm::vec4 frustum[6] = {
        row3 + row0, row3 - row0,   // left, right
        row3 + row1, row3 - row1,   // bottom, top
        row3 + row2, row3 - row2,   // near, far
    };
    for (auto& p : frustum) {
        float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }

    for (auto& entry : ctx.mergedMeshes) {
        if (!entry.mesh) continue; // scene-managed slots can be empty
        if (entry.boundsRadius >= 0.0f) {
            // Distance / fog cull (independent of facing).
            if (fog.enabled) {
                glm::vec3 dd = entry.boundsCenter - camPos;
                dd.y *= 2.0f; // vertical cull range = half horizontal
                float dist2 = glm::dot(dd, dd);
                float threshold = fog.end + entry.boundsRadius;
                if (dist2 > threshold * threshold) continue;
            }
            // Frustum cull: skip chunks fully outside the camera's view volume.
            bool outside = false;
            for (const auto& p : frustum) {
                float dist = glm::dot(glm::vec3(p), entry.boundsCenter) + p.w;
                if (dist < -entry.boundsRadius) { outside = true; break; }
            }
            if (outside) continue;
        }
        entry.mesh->draw(*ctx.shader);
    }

    SceneDef* scene = getActiveScene();
    if (scene && scene->onRender)
        scene->onRender();
}
