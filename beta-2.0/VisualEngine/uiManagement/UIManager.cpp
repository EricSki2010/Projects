#include "UIManager.h"
#include "UIRenderer.h"
#include "TextRenderer.h"
#include "../EngineGlobals.h"
#include <algorithm>
#include <vector>
#include <string>

static std::vector<UIGroup> sGroups;
static bool sWasLeftDown = false;
static bool sKeyStates[GLFW_KEY_LAST + 1] = {};

// Confirmation system
static std::string sPendingConfirmId;
static std::function<void()> sPendingAction;
static std::string sPendingElementId;

// Hover tracking — updated each frame by updateHover().
static int sHoveredGroupIdx = -1;
static int sHoveredElementIdx = -1;

static bool isHoverable(const UIElement& e) {
    return e.hoverable && e.visible && !e.isTextInput;
}

static void inflateForHover(UIElement& e) {
    float dx = e.size.x * 0.025f;
    float dy = e.size.y * 0.025f;
    e.position.x -= dx;
    e.position.y -= dy;
    e.size *= 1.05f;
    e.color.r = std::min(1.0f, e.color.r + 0.1f);
    e.color.g = std::min(1.0f, e.color.g + 0.1f);
    e.color.b = std::min(1.0f, e.color.b + 0.1f);
    e.labelScale *= 1.05f;
}

// Find the currently focused text input across all groups
static UIElement* getFocusedInput() {
    for (auto& g : sGroups) {
        if (!g.visible) continue;
        for (auto& e : g.elements)
            if (e.isTextInput && e.focused) return &e;
    }
    return nullptr;
}

bool isAnyInputFocused() {
    return getFocusedInput() != nullptr;
}

// Forward declarations for helpers defined later in this file.
static float getCorrectedWidth(const UIElement& e);

// Word-wrap `text` so each line fits within `usableW` pixels at the given
// labelScale. Falls back to a hard-character break for words longer than
// usableW. Always returns at least one line (possibly empty).
static std::vector<std::string> wrapTextToWidth(const std::string& text, float usableW, float scale) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto widthOf = [&](const std::string& s) { return measureText(s, scale); };
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string candidate = cur.empty() ? word : (cur + " " + word);
        if (widthOf(candidate) <= usableW) {
            cur = candidate;
        } else {
            if (!cur.empty()) lines.push_back(cur);
            while (widthOf(word) > usableW && word.size() > 1) {
                size_t lo = 1, hi = word.size();
                while (lo < hi) {
                    size_t mid = (lo + hi + 1) / 2;
                    if (widthOf(word.substr(0, mid)) <= usableW) lo = mid;
                    else hi = mid - 1;
                }
                lines.push_back(word.substr(0, lo));
                word = word.substr(lo);
            }
            cur = word;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') { flushWord(); if (!cur.empty()) { lines.push_back(cur); cur.clear(); } }
        else if (c == ' ' || c == '\t') { flushWord(); }
        else word += c;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    if (lines.empty()) lines.push_back("");
    return lines;
}

int inputWrappedLineCount(const UIElement& e) {
    const float padding = 8.0f;
    float pixW = getCorrectedWidth(e) / 2.0f * ctx.width;
    float usableW = pixW - 2.0f * padding;
    std::string display = e.inputText;
    auto lines = wrapTextToWidth(display, usableW, e.labelScale);
    return std::max(1, (int)lines.size());
}

// Unfocus all text inputs
static void unfocusAll() {
    for (auto& g : sGroups)
        for (auto& e : g.elements)
            if (e.isTextInput && e.focused) {
                e.focused = false;
                if (e.onUnfocus) e.onUnfocus(e.inputText);
            }
}

void addUIGroup(const std::string& groupId, bool visible) {
    for (const auto& g : sGroups)
        if (g.id == groupId) return;
    sGroups.push_back({groupId, visible, {}});
}

void removeUIGroup(const std::string& groupId) {
    sGroups.erase(
        std::remove_if(sGroups.begin(), sGroups.end(),
            [&](const UIGroup& g) { return g.id == groupId; }),
        sGroups.end()
    );
}

void addToGroup(const std::string& groupId, const UIElement& element) {
    for (auto& g : sGroups) {
        if (g.id == groupId) {
            g.elements.push_back(element);
            return;
        }
    }
}

void removeFromGroup(const std::string& groupId, const std::string& elementId) {
    for (auto& g : sGroups) {
        if (g.id == groupId) {
            g.elements.erase(
                std::remove_if(g.elements.begin(), g.elements.end(),
                    [&](const UIElement& e) { return e.id == elementId; }),
                g.elements.end()
            );
            return;
        }
    }
}

void setGroupVisible(const std::string& groupId, bool visible) {
    for (auto& g : sGroups) {
        if (g.id == groupId) {
            g.visible = visible;
            return;
        }
    }
}

UIElement* getUIElement(const std::string& groupId, const std::string& elementId) {
    for (auto& g : sGroups) {
        if (g.id == groupId) {
            for (auto& e : g.elements)
                if (e.id == elementId) return &e;
            return nullptr;
        }
    }
    return nullptr;
}

void clearUI() {
    sGroups.clear();
}

// Convert normalized coords to pixel coords for text rendering
static float getCorrectedWidth(const UIElement& e) {
    if (e.aspectCorrected && ctx.width > 0) {
        float aspect = (float)ctx.width / (float)ctx.height;
        return e.size.x / aspect;
    }
    return e.size.x;
}

static void normToPixel(const UIElement& e, float& pixX, float& pixY, float& pixW, float& pixH) {
    pixX = (e.position.x + 1.0f) / 2.0f * ctx.width;
    pixY = (1.0f - e.position.y) / 2.0f * ctx.height;
    pixW = getCorrectedWidth(e) / 2.0f * ctx.width;
    pixH = e.size.y / 2.0f * ctx.height;
}

// Hover-inflated AABB hit test for hoverable elements (5% growth, center-anchored).
static bool hoverContainsPoint(const UIElement& e, glm::vec2 norm) {
    float corrW = getCorrectedWidth(e);
    float dx = corrW * 0.025f;
    float dy = e.size.y * 0.025f;
    return norm.x >= e.position.x - dx && norm.x <= e.position.x + corrW + dx &&
           norm.y >= e.position.y - dy && norm.y <= e.position.y + e.size.y + dy;
}

void renderUI() {
    for (int gi = 0; gi < (int)sGroups.size(); gi++) {
        const auto& g = sGroups[gi];
        if (!g.visible) continue;
        for (int ei = 0; ei < (int)g.elements.size(); ei++) {
            const auto& e = g.elements[ei];
            bool isPending = !sPendingConfirmId.empty() && e.requireConfirm &&
                             e.id == sPendingElementId;
            bool isFocusedInput = e.isTextInput && e.focused;
            bool isHovered = gi == sHoveredGroupIdx && ei == sHoveredElementIdx;

            UIElement drawCopy = e;
            if (isFocusedInput || isPending) {
                drawCopy.color.r = std::min(1.0f, drawCopy.color.r + 0.15f);
                drawCopy.color.g = std::min(1.0f, drawCopy.color.g + 0.15f);
                drawCopy.color.b = std::min(1.0f, drawCopy.color.b + 0.15f);
            }
            if (isHovered) inflateForHover(drawCopy);

            drawUIElement(drawCopy);

            // Draw label (buttons)
            if (!drawCopy.label.empty() && !drawCopy.isTextInput) {
                float pixX, pixY, pixW, pixH;
                normToPixel(drawCopy, pixX, pixY, pixW, pixH);
                float textW = measureText(drawCopy.label, drawCopy.labelScale);
                float textH = measureTextHeight(drawCopy.labelScale);
                drawText(drawCopy.label,
                    pixX + pixW / 2.0f - textW / 2.0f,
                    pixY - pixH / 2.0f - textH / 2.0f,
                    drawCopy.labelScale, drawCopy.labelColor);
            }

            // Draw text input content
            if (drawCopy.isTextInput) {
                float pixX, pixY, pixW, pixH;
                normToPixel(drawCopy, pixX, pixY, pixW, pixH);
                float textH = measureTextHeight(drawCopy.labelScale);
                float padding = 8.0f;

                if (drawCopy.inputText.empty() && !drawCopy.focused) {
                    drawText(drawCopy.placeholder, pixX + padding,
                        pixY - pixH / 2.0f - textH / 2.0f,
                        drawCopy.labelScale, {0.5f, 0.5f, 0.5f, 0.7f});
                } else if (!drawCopy.multiline) {
                    std::string display = drawCopy.inputText;
                    if (drawCopy.focused) {
                        int cp = drawCopy.caretPos;
                        if (cp < 0) cp = 0;
                        if (cp > (int)display.size()) cp = (int)display.size();
                        double time = glfwGetTime();
                        bool on = ((int)(time * 2.0)) % 2 == 0;
                        // Always reserve the caret slot so chars to the right
                        // don't shift between blink phases.
                        display.insert(display.begin() + cp, on ? '|' : ' ');
                    }
                    drawText(display, pixX + padding,
                        pixY - pixH / 2.0f - textH / 2.0f,
                        drawCopy.labelScale, drawCopy.labelColor);
                } else {
                    // Multi-line: use shared wrapTextToWidth so the line
                    // count matches what inputWrappedLineCount reports to
                    // the scene (which sizes the element accordingly).
                    std::string display = drawCopy.inputText;
                    if (drawCopy.focused) {
                        int cp = drawCopy.caretPos;
                        if (cp < 0) cp = 0;
                        if (cp > (int)display.size()) cp = (int)display.size();
                        double time = glfwGetTime();
                        bool on = ((int)(time * 2.0)) % 2 == 0;
                        display.insert(display.begin() + cp, on ? '|' : ' ');
                    }
                    const float usableW = pixW - 2.0f * padding;
                    auto lines = wrapTextToWidth(display, usableW, drawCopy.labelScale);
                    const float lineStep = textH + 4.0f;
                    // Stack lines top-down. pixY is the element's bottom edge
                    // in pixel space (y grows downward); the top edge is at
                    // pixY - pixH. drawText takes y as the line's top.
                    float topInPixels = pixY - pixH + padding;
                    for (size_t i = 0; i < lines.size(); i++) {
                        float y = topInPixels + (float)i * lineStep;
                        drawText(lines[i], pixX + padding, y,
                            drawCopy.labelScale, drawCopy.labelColor);
                    }
                }
            }
        }
    }
}

// Convert screen pixel coords to normalized (-1 to 1)
static glm::vec2 screenToNorm(double mx, double my, int w, int h) {
    return glm::vec2(
        (float)(mx / w) * 2.0f - 1.0f,
        1.0f - (float)(my / h) * 2.0f
    );
}

static void updateHover() {
    double mx, my;
    glfwGetCursorPos(ctx.window, &mx, &my);
    glm::vec2 norm = screenToNorm(mx, my, ctx.width, ctx.height);
    sHoveredGroupIdx = -1;
    sHoveredElementIdx = -1;
    for (int gi = (int)sGroups.size() - 1; gi >= 0; gi--) {
        if (!sGroups[gi].visible) continue;
        for (int ei = (int)sGroups[gi].elements.size() - 1; ei >= 0; ei--) {
            const UIElement& e = sGroups[gi].elements[ei];
            if (!isHoverable(e)) continue;
            if (hoverContainsPoint(e, norm)) {
                sHoveredGroupIdx = gi;
                sHoveredElementIdx = ei;
                return;
            }
        }
    }
}

// Buffer of printable characters typed since last drain. Filled by GLFW's
// character callback (which respects shift, layout, dead keys, etc.) and
// drained into the focused input each frame in processTextInput().
static std::string sCharBuffer;
static bool sCharCallbackInstalled = false;

static void uiCharCallback(GLFWwindow* /*win*/, unsigned int codepoint) {
    // Printable ASCII only. Wider unicode can be added later if needed.
    if (codepoint >= 0x20 && codepoint <= 0x7E)
        sCharBuffer.push_back((char)codepoint);
}

static void processTextInput() {
    if (!sCharCallbackInstalled && ctx.window) {
        glfwSetCharCallback(ctx.window, uiCharCallback);
        sCharCallbackInstalled = true;
    }

    UIElement* input = getFocusedInput();
    if (!input) {
        // Don't queue keystrokes when no input is focused.
        sCharBuffer.clear();
        return;
    }

    // Clamp caret in case inputText was mutated externally since last frame.
    if (input->caretPos < 0) input->caretPos = 0;
    if (input->caretPos > (int)input->inputText.size())
        input->caretPos = (int)input->inputText.size();

    for (char c : sCharBuffer) {
        if ((int)input->inputText.size() >= input->maxLength) break;
        if (input->numericOnly) {
            if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) continue;
        }
        input->inputText.insert(input->inputText.begin() + input->caretPos, c);
        input->caretPos++;
    }
    sCharBuffer.clear();

    // Backspace stays on key-poll: GLFW's char callback only emits printables.
    {
        bool down = glfwGetKey(ctx.window, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
        if (down && !sKeyStates[GLFW_KEY_BACKSPACE] && input->caretPos > 0) {
            input->inputText.erase(input->inputText.begin() + (input->caretPos - 1));
            input->caretPos--;
        }
        sKeyStates[GLFW_KEY_BACKSPACE] = down;
    }

    // Left/Right move caret within the input.
    {
        bool down = glfwGetKey(ctx.window, GLFW_KEY_LEFT) == GLFW_PRESS;
        if (down && !sKeyStates[GLFW_KEY_LEFT] && input->caretPos > 0)
            input->caretPos--;
        sKeyStates[GLFW_KEY_LEFT] = down;
    }
    {
        bool down = glfwGetKey(ctx.window, GLFW_KEY_RIGHT) == GLFW_PRESS;
        if (down && !sKeyStates[GLFW_KEY_RIGHT] &&
            input->caretPos < (int)input->inputText.size())
            input->caretPos++;
        sKeyStates[GLFW_KEY_RIGHT] = down;
    }
}

void processUIInput() {
    updateHover();

    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (leftDown && !sWasLeftDown) {
        double mx, my;
        glfwGetCursorPos(ctx.window, &mx, &my);
        handleUIClick(mx, my, ctx.width, ctx.height);
    }
    sWasLeftDown = leftDown;

    processTextInput();
}

bool handleUIClick(double mouseX, double mouseY, int screenWidth, int screenHeight) {
    glm::vec2 norm = screenToNorm(mouseX, mouseY, screenWidth, screenHeight);

    unfocusAll();

    for (int gi = (int)sGroups.size() - 1; gi >= 0; gi--) {
        UIGroup& g = sGroups[gi];
        if (!g.visible) continue;

        for (int ei = (int)g.elements.size() - 1; ei >= 0; ei--) {
            UIElement& e = g.elements[ei];
            if (!e.visible) continue;

            float corrW = getCorrectedWidth(e);
            bool inside;
            if (e.hoverable) {
                inside = hoverContainsPoint(e, norm);
            } else {
                inside = norm.x >= e.position.x && norm.x <= e.position.x + corrW &&
                         norm.y >= e.position.y && norm.y <= e.position.y + e.size.y;
            }
            if (inside) {

                if (e.isTextInput) {
                    e.focused = true;
                    e.caretPos = (int)e.inputText.size();
                    cancelPendingConfirm();
                    return true;
                }

                // Button requires confirmation — set or replace pending
                if (e.requireConfirm && !e.confirmId.empty()) {
                    sPendingConfirmId = e.confirmId;
                    sPendingAction = e.onClick;
                    sPendingElementId = e.id;
                    return true;
                }

                // Check if this button confirms a pending action
                // (only non-requireConfirm buttons with matching confirmId)
                if (!sPendingConfirmId.empty() && !e.requireConfirm &&
                    !e.confirmId.empty() && e.confirmId == sPendingConfirmId) {
                    auto action = std::move(sPendingAction);
                    cancelPendingConfirm();
                    if (action) action();
                    return true;
                }

                // Normal button — cancel any pending and run
                cancelPendingConfirm();
                if (e.onClick) {
                    auto action = e.onClick;
                    action();
                }
                return true;
            }
        }
    }

    // Clicked empty space — cancel pending
    cancelPendingConfirm();
    return false;
}

bool isPointOverUI(double mouseX, double mouseY, int screenWidth, int screenHeight) {
    glm::vec2 norm = screenToNorm(mouseX, mouseY, screenWidth, screenHeight);
    for (int gi = (int)sGroups.size() - 1; gi >= 0; gi--) {
        const UIGroup& g = sGroups[gi];
        if (!g.visible) continue;
        for (int ei = (int)g.elements.size() - 1; ei >= 0; ei--) {
            const UIElement& e = g.elements[ei];
            if (!e.visible) continue;
            // Skip decorative elements (panels, images with no click behavior).
            bool interactive = (bool)e.onClick || e.isTextInput ||
                               e.requireConfirm || !e.confirmId.empty();
            if (!interactive) continue;
            float corrW = getCorrectedWidth(e);
            bool inside;
            if (e.hoverable) {
                inside = hoverContainsPoint(e, norm);
            } else {
                inside = norm.x >= e.position.x && norm.x <= e.position.x + corrW &&
                         norm.y >= e.position.y && norm.y <= e.position.y + e.size.y;
            }
            if (inside) return true;
        }
    }
    return false;
}

bool hasPendingConfirm() {
    return !sPendingConfirmId.empty();
}

std::string getPendingConfirmId() {
    return sPendingConfirmId;
}

void cancelPendingConfirm() {
    sPendingConfirmId.clear();
    sPendingAction = nullptr;
    sPendingElementId.clear();
}

std::string getInputText(const std::string& groupId, const std::string& elementId) {
    UIElement* e = getUIElement(groupId, elementId);
    if (e && e->isTextInput) return e->inputText;
    return "";
}

void createDropdown(const std::string& groupId, const std::string& id,
                    float x, float y, float w, float h,
                    const glm::vec4& color, const std::string& label,
                    const std::vector<std::string>& options,
                    std::function<void(int index, const std::string& option)> onSelect,
                    float offsetX, float offsetY) {
    std::string dropGroupId = id + "_dropdown";

    // Capture what we need for the toggle and option callbacks
    auto optionsCopy = options;
    auto onSelectCopy = onSelect;

    // Main toggle button
    UIElement btn = UIElement();
    btn.id = id;
    btn.position = glm::vec2(x, y);
    btn.size = glm::vec2(w, h);
    btn.color = color;
    btn.label = label;
    btn.hoverable = true;
    btn.cornerRadius = 8.0f;
    btn.onClick = [dropGroupId, id, x, y, w, h, color, offsetX, offsetY,
                   optionsCopy, onSelectCopy, groupId]() {
        // Toggle: if group exists, remove it; otherwise create it
        bool exists = false;
        for (const auto& g : sGroups)
            if (g.id == dropGroupId) { exists = true; break; }

        if (exists) {
            removeUIGroup(dropGroupId);
            return;
        }

        addUIGroup(dropGroupId);
        float optY = y + offsetY;
        float optX = x + offsetX;
        glm::vec4 optColor = glm::vec4(
            std::min(color.r * 1.2f, 1.0f),
            std::min(color.g * 1.2f, 1.0f),
            std::min(color.b * 1.2f, 1.0f),
            color.a);

        for (int i = 0; i < (int)optionsCopy.size(); i++) {
            std::string optId = dropGroupId + "_" + std::to_string(i);
            std::string optLabel = optionsCopy[i];
            int idx = i;
            auto cb = onSelectCopy;
            std::string dgid = dropGroupId;
            std::string mainBtnGroup = groupId;
            std::string mainBtnId = id;

            UIElement opt = UIElement();
            opt.id = optId;
            opt.position = glm::vec2(optX, optY);
            opt.size = glm::vec2(w, h);
            opt.color = optColor;
            opt.label = optLabel;
            opt.hoverable = true;
            opt.cornerRadius = 8.0f;
            opt.onClick = [cb, idx, optLabel, dgid, mainBtnGroup, mainBtnId]() {
                // Update the main button label before removing anything
                UIElement* mainBtn = getUIElement(mainBtnGroup, mainBtnId);
                if (mainBtn) mainBtn->label = optLabel;
                // Copy callback and args before removing group (which invalidates 'this')
                auto callback = cb;
                int capturedIdx = idx;
                std::string capturedLabel = optLabel;
                std::string capturedDgid = dgid;
                removeUIGroup(capturedDgid);
                if (callback) callback(capturedIdx, capturedLabel);
            };
            addToGroup(dropGroupId, opt);
            optY += offsetY;
        }
    };

    addToGroup(groupId, btn);
}
