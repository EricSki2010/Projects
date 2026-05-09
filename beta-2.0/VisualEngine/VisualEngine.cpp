#include "VisualEngine.h"
#include "EngineGlobals.h"
#include "sceneManagement/SceneManager.h"
#include "renderingManagement/DefaultShaders.h"
#include "renderingManagement/RenderLoop.h"
#include "renderingManagement/effects/GradientBackground.h"
#include "inputManagement/Camera.h"
#include "inputManagement/Collision.h"
#include <iostream>
#include <filesystem>

EngineContext ctx;

static void scrollCallback(GLFWwindow*, double xoffset, double yoffset) {
    ctx.scrollDelta += (float)yoffset;
}

static void framebufferSizeCallback(GLFWwindow*, int width, int height) {
    glViewport(0, 0, width, height);
    ctx.width = width;
    ctx.height = height;
    if (ctx.scene)
        ctx.scene->projection = glm::perspective(glm::radians(45.0f), (float)width / (float)height, 0.1f, 500.0f);
}

namespace VE {

bool initWindow(int width, int height, const char* title, bool maximized) {
    ctx.width = width;
    ctx.height = height;

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4); // 4x MSAA

    ctx.window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!ctx.window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(ctx.window);
    glfwSwapInterval(1); // vsync on: cap FPS to display refresh rate
    if (maximized)
        glfwMaximizeWindow(ctx.window);
    glfwGetFramebufferSize(ctx.window, &ctx.width, &ctx.height);

    glfwSetFramebufferSizeCallback(ctx.window, framebufferSizeCallback);
    glfwSetCursorPosCallback(ctx.window, Camera::mouseCallback);
    glfwSetScrollCallback(ctx.window, scrollCallback);
    glfwSetInputMode(ctx.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glViewport(0, 0, ctx.width, ctx.height);

    ctx.shader = std::make_unique<Shader>(defaultVertSrc, defaultFragSrc);
    ctx.vcShader = std::make_unique<Shader>(vertexColoredVertSrc, vertexColoredFragSrc);
    ctx.scene = std::make_unique<Scene>((float)ctx.width / (float)ctx.height);
    initGradientBackground();

    return true;
}

void setCamera(float x, float y, float z, float yaw, float pitch) {
    Camera* cam = getGlobalCamera();
    cam->position = glm::vec3(x, y, z);
    cam->yaw = yaw;
    cam->pitch = pitch;
    cam->updateDir();
}

void setMoveSpeed(float speed) {
    getGlobalCamera()->moveSpeed = speed;
}

void loadMesh(const char* name, const MeshDef& def) {
    registerMesh(name, def);
}

void loadMesh(const char* name, const char* meshFilePath) {
    registerMeshFromFile(name, meshFilePath);
}

void loadMeshDir(const char* dirPath) {
    if (!std::filesystem::exists(dirPath)) {
        std::cerr << "loadMeshDir: directory not found: " << dirPath << std::endl;
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
        if (entry.path().extension() == ".mesh") {
            std::string name = entry.path().stem().string();
            registerMeshFromFile(name.c_str(), entry.path().string().c_str());
        }
    }
}

void setMode(MeshMode mode) {
    ctx.mode = mode;
    ctx.needsRebuild = true;
}

static bool sCollidersEnabled = true;

void setCollidersEnabled(bool enabled) {
    sCollidersEnabled = enabled;
}

void reserveBlockCapacity(int approximateBlocks) {
    reserveCullCacheCapacity(approximateBlocks);
}

void draw(const char* meshName, float x, float y, float z,
          float rx, float ry, float rz) {
    if (sCollidersEnabled) {
        // Remove any existing collider at this position (e.g. ghost block)
        removeCollider(x, y, z);
    }
    addDrawInstance(meshName, x, y, z, rx, ry, rz);
    if (sCollidersEnabled) {
        const RegisteredMesh* reg = getRegisteredMesh(meshName);
        if (reg)
            addCollider(meshName, reg->vertices.data(), reg->vertexCount,
                        reg->indices.data(), reg->indexCount, reg->rectangular, x, y, z,
                        reg->floatsPerVertex, rx, ry, rz);
    }
    ctx.needsRebuild = true;
}

void undraw(float x, float y, float z) {
    removeDrawInstance(x, y, z);
    if (sCollidersEnabled) removeCollider(x, y, z);
    ctx.needsRebuild = true;
}

void clearDraws() {
    clearDrawInstances();
    if (sCollidersEnabled) clearColliders();
    ctx.needsRebuild = true;
}

bool hasBlockAt(int x, int y, int z) {
    return hasColliderAt(x, y, z);
}

void rebuild() {
    ctx.mergedMeshes.clear();
    if (ctx.mode == CHUNK) {
        ctx.mergedMeshes = buildMergedMeshes();
    } else {
        ctx.mergedMeshes = buildSingleMeshes();
    }
    ctx.needsRebuild = false;
}

void setGradientBackground(bool enable, glm::vec3 top, glm::vec3 bottom) {
    enableGradientBackground(enable);
    setGradientColors(top, bottom);
}

void setBrightness(float brightness) {
    ctx.shader->use();
    glUniform1f(ctx.shader->loc("brightness"), brightness);
}

void setSpecularStrength(float strength) {
    ctx.shader->use();
    glUniform1f(ctx.shader->loc("specularStrength"), strength);
}

void registerScene(const std::string& name, std::function<void(std::shared_ptr<void>)> onEnter,
                   std::function<void()> onExit,
                   std::function<void(float dt)> onInput,
                   std::function<void()> onUpdate,
                   std::function<void()> onRender) {
    SceneDef def;
    def.onEnter = std::move(onEnter);
    def.onExit = std::move(onExit);
    def.onInput = std::move(onInput);
    def.onUpdate = std::move(onUpdate);
    def.onRender = std::move(onRender);
    ::registerScene(name, def);
}

void setScene(const std::string& name, std::shared_ptr<void> data) {
    setActiveScene(name, std::move(data));
}

void run() {
    if (!ctx.window || !ctx.shader || !ctx.scene) return;

    ctx.scene->uploadStaticUniforms(*ctx.shader);
    if (ctx.vcShader) ctx.scene->uploadStaticUniforms(*ctx.vcShader);
    double lastTime = glfwGetTime();

    rebuild();

    while (!glfwWindowShouldClose(ctx.window)) {
        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;

        processInput(dt);
        update();
        render();

        glfwSwapBuffers(ctx.window);
        glfwPollEvents();
    }

    // Exit active scene before cleanup
    SceneDef* active = getActiveScene();
    if (active && active->onExit)
        active->onExit();

    ctx.mergedMeshes.clear();
    cleanupGradientBackground();
    ctx.scene.reset();
    ctx.shader.reset();
    ctx.vcShader.reset();
    glfwTerminate();

    ctx.window = nullptr;
    clearMeshData();
}

} // namespace VE
