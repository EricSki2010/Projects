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
        ctx.scene->projection = glm::perspective(glm::radians(45.0f), (float)width / (float)height, 0.1f, ctx.scene->farPlane);
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
    if (ctx.mode == CHUNK_VOXEL) {
        // CHUNK_VOXEL ignores meshName/rotation; cell is just "filled".
        setVoxelAt((int)roundf(x), (int)roundf(y), (int)roundf(z), 1);
        ctx.needsRebuild = true;
        return;
    }
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
    if (ctx.mode == CHUNK_VOXEL) {
        setVoxelAt((int)roundf(x), (int)roundf(y), (int)roundf(z), 0);
        ctx.needsRebuild = true;
        return;
    }
    removeDrawInstance(x, y, z);
    if (sCollidersEnabled) removeCollider(x, y, z);
    ctx.needsRebuild = true;
}

void clearDraws() {
    if (ctx.mode == CHUNK_VOXEL) {
        clearAllVoxels();
        if (sCollidersEnabled) clearColliders();
        ctx.needsRebuild = true;
        return;
    }
    clearDrawInstances();
    if (sCollidersEnabled) clearColliders();
    ctx.needsRebuild = true;
}

bool hasBlockAt(int x, int y, int z) {
    if (ctx.mode == CHUNK_VOXEL) return hasVoxelAt(x, y, z);
    return hasColliderAt(x, y, z);
}

void rebuild() {
    ctx.mergedMeshes.clear();
    if (ctx.mode == CHUNK)            ctx.mergedMeshes = buildMergedMeshes();
    else if (ctx.mode == CHUNK_VOXEL) ctx.mergedMeshes = buildVoxelChunkMeshes();
    else                              ctx.mergedMeshes = buildSingleMeshes();
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

static void uploadFogToShader(Shader* sh, const Fog& fog) {
    if (!sh) return;
    sh->use();
    glUniform1i(sh->loc("fogEnabled"), fog.enabled ? 1 : 0);
    glUniform3fv(sh->loc("fogColor"), 1, glm::value_ptr(fog.color));
    glUniform1f(sh->loc("fogStart"), fog.start);
    glUniform1f(sh->loc("fogEnd"), fog.end);
}

static int sCycleKey   = 0;
static int sCycleMod   = 0;
static bool sCycleHeld = false;

static bool isModDown(int modKey) {
    if (modKey == 0) return true;
    if (modKey == GLFW_KEY_LEFT_CONTROL || modKey == GLFW_KEY_RIGHT_CONTROL)
        return glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
            || glfwGetKey(ctx.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (modKey == GLFW_KEY_LEFT_SHIFT || modKey == GLFW_KEY_RIGHT_SHIFT)
        return glfwGetKey(ctx.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
            || glfwGetKey(ctx.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    if (modKey == GLFW_KEY_LEFT_ALT || modKey == GLFW_KEY_RIGHT_ALT)
        return glfwGetKey(ctx.window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS
            || glfwGetKey(ctx.window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    return glfwGetKey(ctx.window, modKey) == GLFW_PRESS;
}

void setSceneCycleHotkey(int key, int modKey) {
    sCycleKey  = key;
    sCycleMod  = modKey;
    sCycleHeld = false;
}

// Called from RenderLoop::processInput each frame, before the active scene's onInput.
void pollSceneCycleHotkey() {
    if (sCycleKey == 0 || !ctx.window) return;
    bool keyDown = glfwGetKey(ctx.window, sCycleKey) == GLFW_PRESS;
    bool combo   = keyDown && isModDown(sCycleMod);
    if (combo && !sCycleHeld)
        cycleToNextScene();
    sCycleHeld = combo;
}

void setFarPlane(float farPlane) {
    if (!ctx.scene) return;
    ctx.scene->farPlane = farPlane;
    float aspect = (float)ctx.width / (float)ctx.height;
    ctx.scene->projection = glm::perspective(glm::radians(45.0f), aspect, 0.1f, farPlane);
}

void setFog(bool enable, glm::vec3 color, float start, float end) {
    if (!ctx.scene) return;
    ctx.scene->fog.enabled = enable;
    ctx.scene->fog.color = color;
    ctx.scene->fog.start = start;
    ctx.scene->fog.end = end;
    uploadFogToShader(ctx.shader.get(), ctx.scene->fog);
    uploadFogToShader(ctx.vcShader.get(), ctx.scene->fog);
    if (ctx.shader) ctx.shader->use();
}

void undrawChunkAt(int chunkX, int chunkY, int chunkZ) {
    glm::ivec3 chunkCoord(chunkX, chunkY, chunkZ);
    if (ctx.mode == CHUNK_VOXEL) {
        clearVoxelChunk(chunkCoord);
        ctx.needsRebuild = true;
        return;
    }
    if (sCollidersEnabled) {
        const std::vector<DrawInstance>* bucket = getChunkInstances(chunkCoord);
        if (bucket) {
            std::vector<glm::vec3> positions;
            positions.reserve(bucket->size());
            for (const auto& d : *bucket) positions.push_back(d.position);
            for (const auto& p : positions) removeCollider(p.x, p.y, p.z);
        }
    }
    clearChunkInstances(chunkCoord);
    ctx.needsRebuild = true;
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

    if (ctx.needsRebuild) rebuild();

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
