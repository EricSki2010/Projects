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

// Active drag in a text input. Set on mouse-down inside an input; cleared on
// mouse-up. While set, mouse motion extends the selection by re-hit-testing.
static int sDragGroupIdx = -1;
static int sDragElementIdx = -1;

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
// Wraps text to a pixel width and also reports the source-text index where
// each output line begins. Spaces and \n consumed by the wrap don't appear in
// any line, so naively summing lines[i].size() misaligns caret/selection math
// at every soft-wrap. srcStarts gives the authoritative source-coord mapping.
struct WrappedLayout {
    std::vector<std::string> lines;
    std::vector<int> srcStarts;
};

static WrappedLayout wrapTextLayout(const std::string& text, float usableW, float scale) {
    WrappedLayout out;
    auto widthOf = [&](const std::string& s) { return measureText(s, scale); };

    std::string cur, word;
    int curStart = 0;
    int wordStart = 0;

    auto pushLine = [&](const std::string& s, int start) {
        out.lines.push_back(s);
        out.srcStarts.push_back(start);
    };

    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string candidate = cur.empty() ? word : (cur + " " + word);
        if (widthOf(candidate) <= usableW) {
            if (cur.empty()) curStart = wordStart;
            cur = candidate;
        } else {
            if (!cur.empty()) pushLine(cur, curStart);
            while (widthOf(word) > usableW && word.size() > 1) {
                size_t lo = 1, hi = word.size();
                while (lo < hi) {
                    size_t mid = (lo + hi + 1) / 2;
                    if (widthOf(word.substr(0, mid)) <= usableW) lo = mid;
                    else hi = mid - 1;
                }
                pushLine(word.substr(0, lo), wordStart);
                word = word.substr(lo);
                wordStart += (int)lo;
            }
            cur = word;
            curStart = wordStart;
        }
        word.clear();
    };

    int srcIdx = 0;
    for (char c : text) {
        if (c == '\n') {
            flushWord();
            if (!cur.empty()) { pushLine(cur, curStart); cur.clear(); }
        } else if (c == ' ' || c == '\t') {
            flushWord();
        } else {
            if (word.empty()) wordStart = srcIdx;
            word += c;
        }
        srcIdx++;
    }
    flushWord();
    if (!cur.empty()) pushLine(cur, curStart);
    if (out.lines.empty()) { out.lines.push_back(""); out.srcStarts.push_back(0); }
    return out;
}

static std::vector<std::string> wrapTextToWidth(const std::string& text, float usableW, float scale) {
    return wrapTextLayout(text, usableW, scale).lines;
}

// Convert a source-text caret index to (lineIdx, colInLine) inside a layout.
// Caret on a consumed space (between two wrapped lines) pins to the end of
// the previous line.
static void layoutCaretLineCol(const WrappedLayout& L, int cp, int& outLine, int& outCol) {
    outLine = 0;
    for (int i = (int)L.lines.size() - 1; i >= 0; i--) {
        if (L.srcStarts[i] <= cp) { outLine = i; break; }
    }
    outCol = cp - L.srcStarts[outLine];
    int maxCol = (int)L.lines[outLine].size();
    if (outCol > maxCol) outCol = maxCol;
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
                e.selAnchor = -1;
                if (e.onUnfocus) e.onUnfocus(e.inputText);
            }
}

static bool hasSelection(const UIElement& e) {
    return e.selAnchor >= 0 && e.selAnchor != e.caretPos;
}

static void getSelRange(const UIElement& e, int& outMin, int& outMax) {
    if (!hasSelection(e)) { outMin = outMax = e.caretPos; return; }
    outMin = std::min(e.selAnchor, e.caretPos);
    outMax = std::max(e.selAnchor, e.caretPos);
}

static void clearSelection(UIElement& e) { e.selAnchor = -1; }

// Erase the selected range (if any). Caret lands at the start of the
// removed span. Returns true if anything was deleted.
static bool deleteSelection(UIElement& e) {
    if (!hasSelection(e)) return false;
    int lo, hi; getSelRange(e, lo, hi);
    e.inputText.erase(e.inputText.begin() + lo, e.inputText.begin() + hi);
    e.caretPos = lo;
    e.selAnchor = -1;
    return true;
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

// Draw a selection highlight rectangle in pixel coords. Converts to NDC
// and emits a UIElement quad through the existing UI shader so blending and
// rounded-corner state stay consistent.
static void drawSelectionRect(float pixLeft, float pixRight,
                              float pixTop, float pixBottom) {
    if (pixRight <= pixLeft || pixBottom <= pixTop) return;
    float nLeft   = pixLeft   / (float)ctx.width  * 2.0f - 1.0f;
    float nRight  = pixRight  / (float)ctx.width  * 2.0f - 1.0f;
    float nTop    = 1.0f - pixTop    / (float)ctx.height * 2.0f;
    float nBottom = 1.0f - pixBottom / (float)ctx.height * 2.0f;
    UIElement r;
    r.position = glm::vec2(nLeft, nBottom);
    r.size = glm::vec2(nRight - nLeft, nTop - nBottom);
    r.color = glm::vec4(0.3f, 0.5f, 1.0f, 0.4f); // soft blue, semi-transparent
    drawUIElement(r);
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
                    float textY = pixY - pixH / 2.0f - textH / 2.0f;

                    // Selection highlight (drawn before text so glyphs sit on top).
                    if (drawCopy.focused && hasSelection(drawCopy)) {
                        int lo, hi; getSelRange(drawCopy, lo, hi);
                        float xLo = pixX + padding + measureText(
                            drawCopy.inputText.substr(0, lo), drawCopy.labelScale);
                        float xHi = pixX + padding + measureText(
                            drawCopy.inputText.substr(0, hi), drawCopy.labelScale);
                        drawSelectionRect(xLo, xHi, textY, textY + textH);
                    }

                    drawText(drawCopy.inputText, pixX + padding, textY,
                        drawCopy.labelScale, drawCopy.labelColor);

                    if (drawCopy.focused) {
                        int cp = drawCopy.caretPos;
                        if (cp < 0) cp = 0;
                        if (cp > (int)drawCopy.inputText.size())
                            cp = (int)drawCopy.inputText.size();
                        double time = glfwGetTime();
                        bool on = ((int)(time * 2.0)) % 2 == 0;
                        if (on) {
                            float caretOffset = measureText(
                                drawCopy.inputText.substr(0, cp),
                                drawCopy.labelScale);
                            // Nudge the caret slightly left so it sits between
                            // glyphs instead of overlapping the next one.
                            drawText("|", pixX + padding + caretOffset - 2.0f, textY,
                                drawCopy.labelScale, glm::vec4(1.0f));
                        }
                    }
                } else {
                    // Multi-line: layout gives lines + per-line source starts so
                    // caret/selection math stays correct across consumed spaces.
                    const float usableW = pixW - 2.0f * padding;
                    auto layout = wrapTextLayout(drawCopy.inputText, usableW,
                        drawCopy.labelScale);
                    const float lineStep = textH + 4.0f;
                    float topInPixels = pixY - pixH + padding;

                    // Selection highlight (per line) — drawn before text.
                    if (drawCopy.focused && hasSelection(drawCopy)) {
                        int selLo, selHi; getSelRange(drawCopy, selLo, selHi);
                        for (size_t i = 0; i < layout.lines.size(); i++) {
                            int lineStart = layout.srcStarts[i];
                            int lineEnd = lineStart + (int)layout.lines[i].size();
                            int lo = std::max(selLo, lineStart);
                            int hi = std::min(selHi, lineEnd);
                            if (lo >= hi) continue;
                            const std::string& line = layout.lines[i];
                            float xLo = pixX + padding + measureText(
                                line.substr(0, lo - lineStart), drawCopy.labelScale);
                            float xHi = pixX + padding + measureText(
                                line.substr(0, hi - lineStart), drawCopy.labelScale);
                            float y = topInPixels + (float)i * lineStep;
                            // Shift down 2px so the highlight aligns with the
                            // visible glyph row in multi-line. Single-line
                            // doesn't need this; keep that alone per user QA.
                            drawSelectionRect(xLo, xHi, y + 2.0f, y + textH + 2.0f);
                        }
                    }

                    for (size_t i = 0; i < layout.lines.size(); i++) {
                        float y = topInPixels + (float)i * lineStep;
                        drawText(layout.lines[i], pixX + padding, y,
                            drawCopy.labelScale, drawCopy.labelColor);
                    }

                    if (drawCopy.focused) {
                        double time = glfwGetTime();
                        bool on = ((int)(time * 2.0)) % 2 == 0;
                        if (on) {
                            int cp = drawCopy.caretPos;
                            if (cp < 0) cp = 0;
                            if (cp > (int)drawCopy.inputText.size())
                                cp = (int)drawCopy.inputText.size();
                            int caretLine, caretCol;
                            layoutCaretLineCol(layout, cp, caretLine, caretCol);
                            float caretOffset = measureText(
                                layout.lines[caretLine].substr(0, caretCol),
                                drawCopy.labelScale);
                            float y = topInPixels + (float)caretLine * lineStep;
                            drawText("|", pixX + padding + caretOffset - 2.0f, y,
                                drawCopy.labelScale, glm::vec4(1.0f));
                        }
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

// Caret hit-test: given a click x in pixel coords and the element rect, find
// the caret index inside e.inputText. Walks chars and splits at glyph midpoints
// so clicking the left half of a char puts the caret before it, right half
// puts it after. Single-line variant.
static int hitTestCaretSingleLine(const UIElement& e, float clickPixX,
                                  float pixX, float padding) {
    const float scale = e.labelScale;
    float originX = pixX + padding;
    if (clickPixX <= originX) return 0;
    float cumulative = 0.0f;
    for (size_t i = 0; i < e.inputText.size(); i++) {
        float charW = measureText(e.inputText.substr(i, 1), scale);
        float midpoint = originX + cumulative + charW * 0.5f;
        if (clickPixX < midpoint) return (int)i;
        cumulative += charW;
    }
    return (int)e.inputText.size();
}

// Multi-line variant — picks the line based on click y, then a column within.
static int hitTestCaretMultiLine(const UIElement& e, float clickPixX, float clickPixY,
                                 float pixX, float pixY, float pixH,
                                 float padding) {
    const float scale = e.labelScale;
    const float usableW = (getCorrectedWidth(e) / 2.0f * ctx.width) - 2.0f * padding;
    auto layout = wrapTextLayout(e.inputText, usableW, scale);
    if (layout.lines.empty()) return 0;
    const float textH = measureTextHeight(scale);
    const float lineStep = textH + 4.0f;
    float topInPixels = pixY - pixH + padding;
    int li = (int)((clickPixY - topInPixels) / lineStep);
    if (li < 0) li = 0;
    if (li >= (int)layout.lines.size()) li = (int)layout.lines.size() - 1;

    int base = layout.srcStarts[li];
    float originX = pixX + padding;
    if (clickPixX <= originX) return base;
    float cumulative = 0.0f;
    const std::string& line = layout.lines[li];
    for (size_t i = 0; i < line.size(); i++) {
        float charW = measureText(line.substr(i, 1), scale);
        float midpoint = originX + cumulative + charW * 0.5f;
        if (clickPixX < midpoint) return base + (int)i;
        cumulative += charW;
    }
    return base + (int)line.size();
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

// Per-key timing for press-and-hold repeat. Indexed by GLFW key code.
static double sKeyDownAt[GLFW_KEY_LAST + 1] = {};
static double sKeyLastFire[GLFW_KEY_LAST + 1] = {};

// Returns true on the press edge, then again after a 400ms initial delay
// and every ~33ms while held — same cadence as the OS text-edit repeat.
static bool shouldFireKeyRepeat(int key) {
    bool down = glfwGetKey(ctx.window, key) == GLFW_PRESS;
    bool wasDown = sKeyStates[key];
    sKeyStates[key] = down;
    if (!down) return false;
    double now = glfwGetTime();
    if (!wasDown) {
        sKeyDownAt[key] = now;
        sKeyLastFire[key] = now;
        return true;
    }
    const double initialDelay = 0.4;
    const double repeatInterval = 0.033;
    if (now - sKeyDownAt[key] < initialDelay) return false;
    if (now - sKeyLastFire[key] >= repeatInterval) {
        sKeyLastFire[key] = now;
        return true;
    }
    return false;
}

// Press-edge only — for one-shot actions like Home/End/Ctrl+V.
static bool pressedThisFrame(int key) {
    bool down = glfwGetKey(ctx.window, key) == GLFW_PRESS;
    bool wasDown = sKeyStates[key];
    sKeyStates[key] = down;
    return down && !wasDown;
}

static bool ctrlHeld() {
    return glfwGetKey(ctx.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
           glfwGetKey(ctx.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

// Insert a single char at the caret if it passes the input's filters.
// Returns true if inserted. Used by typing and paste.
static bool insertCharAtCaret(UIElement* input, char c) {
    if ((int)input->inputText.size() >= input->maxLength) return false;
    if (input->numericOnly) {
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    input->inputText.insert(input->inputText.begin() + input->caretPos, c);
    input->caretPos++;
    return true;
}

// Shift+arrow: ensure selAnchor is set (anchor at current caret if not), then
// move caret. Plain arrow: if there's a selection, collapse to the appropriate
// end and clear; otherwise normal caret move.
static void moveCaretWithSelection(UIElement* input, int newCaret, bool shift) {
    if (newCaret < 0) newCaret = 0;
    if (newCaret > (int)input->inputText.size())
        newCaret = (int)input->inputText.size();
    if (shift) {
        if (input->selAnchor < 0) input->selAnchor = input->caretPos;
        input->caretPos = newCaret;
    } else {
        clearSelection(*input);
        input->caretPos = newCaret;
    }
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

    bool ctrl = ctrlHeld();
    bool shift = glfwGetKey(ctx.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                 glfwGetKey(ctx.window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    // Ctrl+A — select all.
    if (ctrl && pressedThisFrame(GLFW_KEY_A)) {
        input->selAnchor = 0;
        input->caretPos = (int)input->inputText.size();
    }

    // Ctrl+C — copy selection to clipboard. No-op when nothing is selected.
    if (ctrl && pressedThisFrame(GLFW_KEY_C)) {
        if (hasSelection(*input)) {
            int lo, hi; getSelRange(*input, lo, hi);
            std::string sel = input->inputText.substr(lo, hi - lo);
            glfwSetClipboardString(ctx.window, sel.c_str());
        }
    }

    // Ctrl+X — copy selection then delete it.
    if (ctrl && pressedThisFrame(GLFW_KEY_X)) {
        if (hasSelection(*input)) {
            int lo, hi; getSelRange(*input, lo, hi);
            std::string sel = input->inputText.substr(lo, hi - lo);
            glfwSetClipboardString(ctx.window, sel.c_str());
            deleteSelection(*input);
        }
    }

    // Ctrl+V — paste clipboard at caret (replacing any selection). Filtered
    // through the same per-char rules as typing. GLFW returns NUL-terminated
    // UTF-8; non-printable and non-ASCII bytes are dropped (matches the char
    // callback's filter).
    if (ctrl && pressedThisFrame(GLFW_KEY_V)) {
        const char* clip = glfwGetClipboardString(ctx.window);
        if (clip) {
            deleteSelection(*input);
            int pasteStart = input->caretPos;
            for (const char* p = clip; *p; p++) {
                char c = *p;
                if (c == '\r') continue; // normalize CRLF
                if (c == '\n') continue; // single-line for now; multi-line paste TBD
                if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) continue;
                if (!insertCharAtCaret(input, c)) break;
            }
            // Select the just-pasted span so the user can immediately retype
            // or arrow-away to deselect.
            if (input->caretPos > pasteStart)
                input->selAnchor = pasteStart;
        }
    }

    // Drain typed characters. Skip while Ctrl is held so Ctrl+letter combos
    // don't double-fire (GLFW does suppress most on Windows but be defensive).
    if (!ctrl && !sCharBuffer.empty()) {
        deleteSelection(*input); // typing replaces a selection
        for (char c : sCharBuffer) {
            if (!insertCharAtCaret(input, c)) break;
        }
    }
    sCharBuffer.clear();

    // Backspace — delete selection if any, else delete char left of caret.
    if (shouldFireKeyRepeat(GLFW_KEY_BACKSPACE)) {
        if (!deleteSelection(*input) && input->caretPos > 0) {
            input->inputText.erase(input->inputText.begin() + (input->caretPos - 1));
            input->caretPos--;
        }
    }

    // Left / Right — shift extends selection, plain collapses it.
    if (shouldFireKeyRepeat(GLFW_KEY_LEFT)) {
        int target;
        if (!shift && hasSelection(*input)) {
            int lo, hi; getSelRange(*input, lo, hi);
            target = lo;
        } else {
            target = input->caretPos - 1;
        }
        moveCaretWithSelection(input, target, shift);
    }
    if (shouldFireKeyRepeat(GLFW_KEY_RIGHT)) {
        int target;
        if (!shift && hasSelection(*input)) {
            int lo, hi; getSelRange(*input, lo, hi);
            target = hi;
        } else {
            target = input->caretPos + 1;
        }
        moveCaretWithSelection(input, target, shift);
    }

    // Home / End — jump to start/end of field. Shift extends selection.
    if (pressedThisFrame(GLFW_KEY_HOME))
        moveCaretWithSelection(input, 0, shift);
    if (pressedThisFrame(GLFW_KEY_END))
        moveCaretWithSelection(input, (int)input->inputText.size(), shift);
}

void processUIInput() {
    updateHover();

    bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (leftDown && !sWasLeftDown) {
        double mx, my;
        glfwGetCursorPos(ctx.window, &mx, &my);
        handleUIClick(mx, my, ctx.width, ctx.height);
    } else if (leftDown && sDragGroupIdx >= 0 && sDragElementIdx >= 0) {
        // Mouse drag inside a text input — extend selection by re-hit-testing.
        if (sDragGroupIdx < (int)sGroups.size()) {
            auto& grp = sGroups[sDragGroupIdx];
            if (sDragElementIdx < (int)grp.elements.size()) {
                UIElement& e = grp.elements[sDragElementIdx];
                if (e.isTextInput && e.focused) {
                    double mx, my;
                    glfwGetCursorPos(ctx.window, &mx, &my);
                    float pixX, pixY, pixW, pixH;
                    normToPixel(e, pixX, pixY, pixW, pixH);
                    const float padding = 8.0f;
                    int caret;
                    if (e.multiline) {
                        caret = hitTestCaretMultiLine(e, (float)mx, (float)my,
                            pixX, pixY, pixH, padding);
                    } else {
                        caret = hitTestCaretSingleLine(e, (float)mx, pixX, padding);
                    }
                    // First time the caret actually moves during drag, anchor
                    // the selection at where the click started (current caret).
                    if (caret != e.caretPos && e.selAnchor < 0)
                        e.selAnchor = e.caretPos;
                    e.caretPos = caret;
                }
            }
        }
    }
    if (!leftDown) {
        sDragGroupIdx = -1;
        sDragElementIdx = -1;
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
                    float pixX, pixY, pixW, pixH;
                    normToPixel(e, pixX, pixY, pixW, pixH);
                    const float padding = 8.0f;
                    int caret;
                    if (e.multiline) {
                        caret = hitTestCaretMultiLine(e, (float)mouseX, (float)mouseY,
                            pixX, pixY, pixH, padding);
                    } else {
                        caret = hitTestCaretSingleLine(e, (float)mouseX, pixX, padding);
                    }
                    e.caretPos = caret;
                    e.selAnchor = -1; // no selection; drag will create one
                    sDragGroupIdx = gi;
                    sDragElementIdx = ei;
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
