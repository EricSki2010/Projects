#include "SceneManager.h"
#include <vector>

static std::unordered_map<std::string, SceneDef> sScenes;
static std::vector<std::string> sSceneOrder; // registration order, used for cycleToNextScene
static std::string sActiveSceneName;
static SceneDef* sActiveScene = nullptr;

void registerScene(const std::string& name, const SceneDef& scene) {
    if (sScenes.find(name) == sScenes.end())
        sSceneOrder.push_back(name);
    sScenes[name] = scene;
}

void setActiveScene(const std::string& name, std::shared_ptr<void> data) {
    if (sActiveScene && sActiveScene->onExit)
        sActiveScene->onExit();

    auto it = sScenes.find(name);
    if (it != sScenes.end()) {
        sActiveSceneName = name;
        sActiveScene = &it->second;
        if (sActiveScene->onEnter)
            sActiveScene->onEnter(std::move(data));
    }
}

const std::string& getActiveSceneName() {
    return sActiveSceneName;
}

SceneDef* getActiveScene() {
    return sActiveScene;
}

void cycleToNextScene() {
    if (sSceneOrder.size() < 2 || sActiveSceneName.empty()) return;
    for (size_t i = 0; i < sSceneOrder.size(); i++) {
        if (sSceneOrder[i] == sActiveSceneName) {
            const std::string& next = sSceneOrder[(i + 1) % sSceneOrder.size()];
            setActiveScene(next, nullptr);
            return;
        }
    }
}
