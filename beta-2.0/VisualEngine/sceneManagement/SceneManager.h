#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

struct SceneDef {
    std::function<void(std::shared_ptr<void> data)> onEnter;
    std::function<void()> onExit;
    std::function<void(float dt)> onInput;
    std::function<void()> onUpdate;
    std::function<void()> onRender;
};

void registerScene(const std::string& name, const SceneDef& scene);
void setActiveScene(const std::string& name, std::shared_ptr<void> data = nullptr);
const std::string& getActiveSceneName();
SceneDef* getActiveScene();

// Cycles to the next registered scene in registration order (wraps).
// No-op if fewer than 2 scenes are registered or no scene is active.
void cycleToNextScene();
