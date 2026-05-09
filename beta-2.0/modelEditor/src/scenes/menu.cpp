#include "menu.h"
#include "../../../VisualEngine/VisualEngine.h"
#include "../../../VisualEngine/EngineGlobals.h"
#include "../../../VisualEngine/inputManagement/Camera.h"
#include "../../../VisualEngine/uiManagement/UIManager.h"
#include "../../../VisualEngine/uiManagement/UIRenderer.h"
#include "../../../VisualEngine/uiManagement/UIPrefabs.h"
#include "../../../VisualEngine/uiManagement/TextRenderer.h"
#include "../../../VisualEngine/uiManagement/EmbeddedFont.h"
#include "../mechanics/AiHandling/AiHandling.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

static bool sLoadView = false;

static void showMainMenu();
static void showLoadMenu();
static void showFileList(const std::string& type);
static void showSetKeyMenu();

static const float BTN_W = 0.4f;
static const float BTN_H = 0.08f;
static const float BTN_X = -BTN_W / 2.0f;
static const float GAP = 0.12f;
static const float START_Y = 0.1f;
static const glm::vec4 BTN_COLOR = {0.2f, 0.2f, 0.2f, 0.9f};

static void showMainMenu() {
    sLoadView = false;
    removeUIGroup("menu_buttons");
    addUIGroup("menu_buttons");

    float y = START_Y;

    addToGroup("menu_buttons", createButton("create_new",
        BTN_X, y, BTN_W, BTN_H, BTN_COLOR,
        "Create New",
        []() { VE::setScene("createScene"); }
    ));
    y -= GAP;

    addToGroup("menu_buttons", createButton("load",
        BTN_X, y, BTN_W, BTN_H, BTN_COLOR,
        "Load",
        []() { showLoadMenu(); }
    ));
    y -= GAP;

    // AI toggle — placed right above Exit. Rebuilds the menu on click so the
    // label reflects the new state.
    addToGroup("menu_buttons", createButton("ai_toggle",
        BTN_X, y, BTN_W, BTN_H,
        {0.2f, 0.25f, 0.35f, 0.9f},
        AI::isEnabled() ? "AI: ON" : "AI: OFF",
        []() {
            AI::setEnabled(!AI::isEnabled());
            showMainMenu();
        }
    ));
    y -= GAP;

    addToGroup("menu_buttons", createButton("set_api_key",
        BTN_X, y, BTN_W, BTN_H,
        {0.2f, 0.25f, 0.35f, 0.9f},
        "Set API Key",
        []() { showSetKeyMenu(); }
    ));
    y -= GAP;

    addToGroup("menu_buttons", createButton("exit",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Exit",
        []() { glfwSetWindowShouldClose(ctx.window, true); }
    ));
}

static void showSetKeyMenu() {
    removeUIGroup("menu_buttons");
    addUIGroup("menu_buttons");

    float y = START_Y;

    UIElement keyInput = createTextInput("key_input",
        BTN_X, y, BTN_W, BTN_H,
        {0.18f, 0.18f, 0.22f, 0.95f},
        "Paste Gemini API key...", 200);
    addToGroup("menu_buttons", keyInput);
    y -= GAP;

    addToGroup("menu_buttons", createButton("key_save",
        BTN_X, y, BTN_W, BTN_H,
        {0.2f, 0.35f, 0.2f, 0.9f},
        "Save",
        []() {
            std::string key = getInputText("menu_buttons", "key_input");
            // Trim trailing whitespace / newlines from a pasted value.
            while (!key.empty() && (key.back() == '\n' || key.back() == '\r'
                                    || key.back() == ' ' || key.back() == '\t'))
                key.pop_back();
            if (key.empty()) return;

            std::ofstream out(".gemini_key", std::ios::trunc);
            if (out) out << key;
            out.close();

            // Apply to the current process so a restart isn't required.
#ifdef _WIN32
            _putenv_s("GEMINI_API_KEY", key.c_str());
#else
            setenv("GEMINI_API_KEY", key.c_str(), 1);
#endif
            // Restart the sidecar if AI is on so it picks up the new key.
            if (AI::isEnabled()) {
                AI::shutdown();
                AI::init();
            }
            showMainMenu();
        }
    ));
    y -= GAP;

    addToGroup("menu_buttons", createButton("key_back",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Back",
        []() { showMainMenu(); }
    ));
}

static void showLoadMenu() {
    sLoadView = true;
    removeUIGroup("menu_buttons");
    addUIGroup("menu_buttons");

    float y = START_Y;

    createDropdown("menu_buttons", "select_file",
        BTN_X, y, BTN_W, BTN_H, BTN_COLOR,
        "Select File Type...",
        {"3D Model"},
        [](int index, const std::string& option) {
            if (option == "3D Model") showFileList("3dModels");
        },
        0.0f, -GAP
    );
    y -= GAP;

    addToGroup("menu_buttons", createButton("back",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Back",
        []() { showMainMenu(); }
    ));
}

static void showFileList(const std::string& type) {
    removeUIGroup("menu_buttons");
    removeUIGroup("dropdown");
    addUIGroup("menu_buttons");

    float y = START_Y;
    std::string dir = "assets/saves/" + type;

    if (std::filesystem::exists(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".mdl") {
                std::string name = entry.path().stem().string();
                std::string btnId = "file_" + name;

                UIElement fileBtn = createButton(btnId,
                    BTN_X, y, BTN_W, BTN_H, BTN_COLOR,
                    name,
                    [name]() {
                        VE::setScene("3dModeler", std::make_shared<std::string>(name));
                    }
                );
                fileBtn.requireConfirm = true;
                fileBtn.confirmId = "load_file";
                addToGroup("menu_buttons", fileBtn);

                {
                    float subH = BTN_H * 0.8f;
                    float pad = BTN_H * 0.1f;
                    float aspect = (float)ctx.width / (float)ctx.height;
                    float subW = subH / aspect;
                    float subX = BTN_X + BTN_W - subW - pad;
                    float subY = y + pad;

                    float capturedSubX = subX, capturedSubY = subY;
                    float capturedSubW = subW, capturedSubH = subH;
                    UIElement menuBtn = createButton(btnId + "_menu",
                        subX, subY, subW, subH,
                        {0.35f, 0.35f, 0.35f, 1.0f}, ". . .",
                        [name, capturedSubX, capturedSubY, capturedSubW, capturedSubH]() {
                            // Toggle: close if already open
                            if (getUIElement("file_dropdown", "fd_delete")) {
                                removeUIGroup("file_dropdown");
                                return;
                            }

                            addUIGroup("file_dropdown");
                            float dropX = capturedSubX + capturedSubW + capturedSubW * 0.5f;
                            float dropH = BTN_H * 0.7f;
                            float dropGap = dropH + 0.005f;

                            addToGroup("file_dropdown", createButton("fd_delete",
                                dropX, capturedSubY, 0.15f, dropH,
                                {0.5f, 0.1f, 0.1f, 0.95f}, "Delete",
                                [name]() {
                                    std::filesystem::remove("assets/saves/3dModels/" + name + ".mdl");
                                    removeUIGroup("file_dropdown");
                                    showFileList("3dModels");
                                }
                            ));

                            addToGroup("file_dropdown", createButton("fd_duplicate",
                                dropX, capturedSubY - dropGap, 0.15f, dropH,
                                {0.2f, 0.2f, 0.4f, 0.95f}, "Duplicate",
                                [name]() {
                                    std::string src = "assets/saves/3dModels/" + name + ".mdl";
                                    std::string dst = "assets/saves/3dModels/" + name + "_copy.mdl";
                                    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::skip_existing);
                                    removeUIGroup("file_dropdown");
                                    showFileList("3dModels");
                                }
                            ));
                        }
                    );
                    menuBtn.labelScale = 0.2f;
                    addToGroup("menu_buttons", menuBtn);
                }

                y -= GAP;
            }
        }
    }

    // Load button — confirms the file selection
    UIElement loadBtn = createButton("load_confirm",
        BTN_X, y, BTN_W, BTN_H,
        {0.8f, 0.7f, 0.0f, 0.95f},
        "Load",
        nullptr
    );
    loadBtn.confirmId = "load_file";
    addToGroup("menu_buttons", loadBtn);
    y -= GAP;

    addToGroup("menu_buttons", createButton("back",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Back",
        []() { removeUIGroup("file_dropdown"); showLoadMenu(); }
    ));
}

void registerMenuScene() {
    VE::registerScene("menu",
        // onEnter
        [](std::shared_ptr<void> data) {
            sLoadView = false;
            getGlobalCamera()->setMode(CAMERA_FLAT);
            initUIRenderer();
            initTextRendererFromMemory(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE, 48);
            showMainMenu();
        },
        // onExit
        []() {
            cleanupTextRenderer();
            cleanupUIRenderer();
            clearUI();
        },
        // onInput
        [](float dt) {
            processUIInput();
        },
        // onUpdate
        nullptr,
        // onRender
        []() {
            renderUI();
        }
    );
}
