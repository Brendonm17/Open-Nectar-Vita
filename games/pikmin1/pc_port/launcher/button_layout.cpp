#include "button_layout.h"

#include <algorithm>
#include <cmath>

namespace pikmin::launcher::buttons {

namespace {
constexpr float kEpsilon = 1e-3f;

// Quita el último carácter UTF-8 (y los espacios que queden al final).
void popCodepoint(std::string& text)
{
    if (text.empty()) return;
    size_t at = text.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;
    text.erase(at);
    while (!text.empty() && text.back() == ' ') text.pop_back();
}
} // namespace

float capInset(float height, float textHeight)
{
    const float r = height * 0.5f - kButtonBorder;
    if (r <= 0) return 0;
    const float half = std::min(textHeight * 0.5f, r);
    return r - std::sqrt(std::max(0.0f, r * r - half * half));
}

float horizontalInset(float height, float textHeight)
{
    return kButtonBorder + kButtonPadX + capInset(height, textHeight);
}

Rect innerTextRect(float width, float height, float textHeight)
{
    const float x = horizontalInset(height, textHeight);
    const float y = kButtonBorder + kButtonPadY;
    Rect r;
    r.x0 = x;
    r.x1 = std::max(x, width - x);
    r.y0 = y;
    r.y1 = std::max(y, height - y);
    return r;
}

bool labelFits(const std::string& label, float width, float height, float size, const MeasureFn& measure)
{
    const Rect r = innerTextRect(width, height, size);
    return size <= r.height() + kEpsilon && measure(label, size) <= r.width() + kEpsilon;
}

FittedLabel fitLabel(const std::string& label, float width, float height, const MeasureFn& measure, float baseSize,
                     float floorSize, float step)
{
    FittedLabel out;
    out.text = label;
    floorSize = std::min(floorSize, baseSize);
    step = std::max(step, 0.25f);
    for (float size = baseSize;; size = std::max(floorSize, size - step)) {
        if (labelFits(label, width, height, size, measure)) {
            out.size = size;
            out.shrunk = size < baseSize;
            return out;
        }
        if (size <= floorSize) break;
    }
    out.size = floorSize;
    out.shrunk = floorSize < baseSize;
    out.truncated = true;
    const float room = innerTextRect(width, height, floorSize).width() + kEpsilon;
    std::string text = label;
    while (!text.empty()) {
        popCodepoint(text);
        if (!text.empty() && measure(text + kEllipsis, floorSize) <= room) {
            out.text = text + kEllipsis;
            return out;
        }
    }
    out.text = measure(kEllipsis, floorSize) <= room ? kEllipsis : "";
    return out;
}

ButtonSize sizeFor(const std::string& label, float height, const MeasureFn& measure, ButtonSize minimum, float baseSize)
{
    for (ButtonSize size : kAllSizes) {
        if (static_cast<int>(size) < static_cast<int>(minimum)) continue;
        if (labelFits(label, buttonWidth(size), height, baseSize, measure)) return size;
    }
    return ButtonSize::ExtraLarge;
}

float fitWidth(const std::string& label, float height, float size, const MeasureFn& measure)
{
    return std::ceil(measure(label, size) + 2.0f * horizontalInset(height, size));
}

float gameGridWidth()
{
    return 2.0f * buttonWidth(role::kGameTool) + kButtonGap;
}

float gameCardGap(float cardW)
{
    return std::max(cardW * 0.42f, gameGridWidth() + kGameGridGutter - cardW);
}

} // namespace pikmin::launcher::buttons
