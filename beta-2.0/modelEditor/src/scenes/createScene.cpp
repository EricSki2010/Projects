#include "createScene.h"
#include "../../../VisualEngine/VisualEngine.h"
#include "../../../VisualEngine/EngineGlobals.h"
#include "../../../VisualEngine/inputManagement/Camera.h"
#include "../../../VisualEngine/uiManagement/UIManager.h"
#include "../../../VisualEngine/uiManagement/UIRenderer.h"
#include "../../../VisualEngine/uiManagement/UIPrefabs.h"
#include "../../../VisualEngine/uiManagement/TextRenderer.h"
#include "../../../VisualEngine/uiManagement/EmbeddedFont.h"
#include "../../../VisualEngine/memoryManagement/memory.h"
#include "../prefabs/templates.h"
#include <memory>


static std::string sSelectedType;
static std::string sSelectedTemplate;
static std::string sSavedName;
static std::string sSavedCubeSize;
static bool sTypeSelected = false;
static bool sPickingTemplate = false;

static const float BTN_X = -0.2f;
static const float BTN_W = 0.4f;
static const float BTN_H = 0.07f;
static const float GAP = 0.09f;
static const float START_Y = 0.15f;

static void rebuildLayout();
static void showTemplatePicker();
static void selectTemplate(const std::string& tmpl);
static void saveInputs();


static void selectTemplate(const std::string& tmpl) {
    sSelectedTemplate = tmpl;
    sPickingTemplate = false;
    rebuildLayout();
}

static void saveInputs() {
    std::string name = getInputText("create_ui", "name_input");
    if (!name.empty()) sSavedName = name;
    std::string cubeSize = getInputText("create_ui", "cube_size");
    if (!cubeSize.empty()) sSavedCubeSize = cubeSize;
}

static void showTemplatePicker() {
    saveInputs();
    sPickingTemplate = true;
    removeUIGroup("create_ui");
    addUIGroup("create_ui");

    float y = START_Y;

    addToGroup("create_ui", createButton("tmpl_none",
        BTN_X, y, BTN_W, BTN_H,
        {0.2f, 0.2f, 0.2f, 0.95f},
        "No Template",
        []() { selectTemplate("None"); }
    ));
    y -= GAP;

    addToGroup("create_ui", createButton("tmpl_cube",
        BTN_X, y, BTN_W, BTN_H,
        {0.2f, 0.2f, 0.2f, 0.95f},
        "Starting Cube",
        []() {
            selectTemplate("Starting Cube");
        }
    ));
    y -= GAP;

    addToGroup("create_ui", createButton("tmpl_back",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Back",
        []() {
            sPickingTemplate = false;
            rebuildLayout();
        }
    ));
}

static void rebuildLayout() {
    removeUIGroup("create_ui");
    addUIGroup("create_ui");

    float y = START_Y;

    // Type dropdown
    std::string dropLabel = sTypeSelected ? sSelectedType : "Select Type...";
    createDropdown("create_ui", "dropdown_btn",
        BTN_X, y, BTN_W, BTN_H,
        {0.15f, 0.15f, 0.15f, 0.95f},
        dropLabel,
        {"3D Model"},
        [](int index, const std::string& option) {
            saveInputs();
            sSelectedType = option;
            sTypeSelected = true;
            rebuildLayout();
        },
        0.0f, -GAP
    );
    y -= GAP;

    // Name input + Create (only if type selected)
    if (sTypeSelected) {
        {
            UIElement nameInput = createTextInput("name_input",
                BTN_X, y, BTN_W, BTN_H,
                {0.1f, 0.1f, 0.1f, 0.95f},
                "Enter name...", 24);
            nameInput.inputText = sSavedName;
            addToGroup("create_ui", nameInput);
        }
        y -= GAP;

        std::string tmplLabel = sSelectedTemplate.empty() ? "Add Template" : "Template: " + sSelectedTemplate;
        addToGroup("create_ui", createButton("template_btn",
            BTN_X, y, BTN_W, BTN_H,
            {0.2f, 0.2f, 0.3f, 0.95f},
            tmplLabel,
            []() { showTemplatePicker(); }
        ));
        y -= GAP;

        if (sSelectedTemplate == "Starting Cube") {
            UIElement sizeInput = createTextInput("cube_size",
                BTN_X, y, BTN_W, BTN_H,
                {0.1f, 0.1f, 0.1f, 0.95f},
                "Cube size (e.g. 3)", 6);
            sizeInput.inputText = sSavedCubeSize;
            sizeInput.onUnfocus = [](std::string& text) {
                if (text.empty()) return;
                try {
                    int val = std::stoi(text);
                    if (val < 1) val = 1;
                    if (val > 64) val = 64;
                    text = std::to_string(val);
                } catch (...) {
                    text = "1";
                }
            };
            addToGroup("create_ui", sizeInput);
            y -= GAP;
        }

        addToGroup("create_ui", createButton("create_btn",
            BTN_X, y, BTN_W, BTN_H,
            {0.1f, 0.4f, 0.15f, 0.95f},
            "Create",
            []() {
                std::string name = getInputText("create_ui", "name_input");
                if (name.empty() || sSelectedType.empty()) return;

                if (sSelectedType == "3D Model") {
                    setMemoryPath("assets/saves/3dModels");

                    ModelFile model;

                    if (sSelectedTemplate == "Starting Cube") {
                        std::string sizeStr = getInputText("create_ui", "cube_size");
                        int size = sizeStr.empty() ? 1 : std::stoi(sizeStr);
                        model = cubeTemplate(size);
                    }

                    saveModel(name, model);
                    VE::setScene("3dModeler", std::make_shared<std::string>(name));
                }
            }
        ));
        y -= GAP;
    }

    // Back button always last
    addToGroup("create_ui", createButton("back_btn",
        BTN_X, y, BTN_W, BTN_H,
        {0.3f, 0.1f, 0.1f, 0.9f},
        "Back",
        []() { VE::setScene("menu"); }
    ));
}

void registerCreateScene() {
    VE::registerScene("createScene",
        // onEnter
        [](std::shared_ptr<void> data) {
            sSelectedType = "";
            sSelectedTemplate = "";
            sSavedName = "";
            sSavedCubeSize = "";

            sTypeSelected = false;
            sPickingTemplate = false;
            getGlobalCamera()->setMode(CAMERA_FLAT);
            initUIRenderer();
            initTextRendererFromMemory(EMBEDDED_FONT_DATA, EMBEDDED_FONT_SIZE, 48);
            rebuildLayout();
        },
        // onExit
        []() {

            sSelectedType = "";
            sSelectedTemplate = "";
            sTypeSelected = false;
            sPickingTemplate = false;
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
