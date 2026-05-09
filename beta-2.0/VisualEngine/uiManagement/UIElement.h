#pragma once

#include <glm/glm.hpp>
#include <string>
#include <functional>

struct UIElement {
    std::string id;
    glm::vec2 position;     // normalized screen-space (-1 to 1)
    glm::vec2 size;         // width/height in normalized coords
    glm::vec4 color;        // RGBA
    unsigned int textureId = 0; // GL texture ID, 0 = no texture
    std::string label;      // text to display centered on element
    float labelScale = 0.3f;
    glm::vec4 labelColor = glm::vec4(1.0f);
    std::function<void()> onClick;
    bool visible = true;
    // When true, element grows + lightens on hover, and click bounds match the inflated rect.
    bool hoverable = false;

    // Text input fields
    bool isTextInput = false;
    bool focused = false;
    std::string inputText;
    std::string placeholder;
    int maxLength = 32;
    std::function<void(std::string&)> onUnfocus;
    // Multi-line: text wraps within the input's width, and the element's
    // size.y is expected to be resized by the caller to fit the line count.
    bool multiline = false;
    // When true, the text input only accepts digits, '.', and '-'.
    bool numericOnly = false;
    // When true, the text input only accepts hex digits (0-9, a-f, A-F).
    // Non-hex chars are silently dropped on type and paste.
    bool hexOnly = false;
    // Caret position within inputText (0..inputText.size()). Snapped to end
    // on focus; moved by Left/Right arrows; insertions/deletions happen here.
    int caretPos = 0;
    // Selection anchor. -1 means "no selection". When set, the selected range
    // is [min(selAnchor, caretPos), max(selAnchor, caretPos)]. The caret is
    // always at one end of the selection.
    int selAnchor = -1;

    // When true, width is adjusted by aspect ratio so equal w/h values render as a square
    bool aspectCorrected = false;

    // Confirmation system
    bool requireConfirm = false;
    std::string confirmId;

    // Rounded corners in pixels. 0 = sharp rectangle. Set by clickable-button prefabs.
    float cornerRadius = 0.0f;
};
