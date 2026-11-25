// Dock drawing shared between platforms (see dock_drawing.h). Moved from src/shell/dock.cpp with the same arithmetic:
// on Windows the result is identical, pixel for pixel.
#include "dock_drawing.h"
#include <algorithm>

void dock_drawing::Badge(drawing::Painter& p, const drawing::Rect& ib, int n) {
    using namespace drawing;
    float body = ib.right - ib.left;
    float cx = (ib.left + ib.right) / 2, cy = (ib.top + ib.bottom) / 2;
    // measured on the Mac (Messages' icon): a circle of ~44% of the icon, ~13% past the right edge and ~10% past the
    // top; a light number of ~55% of the circle
    float d = body * 0.44f;
    std::wstring t = n > 99 ? L"99+" : std::to_wstring(n);
    float tw = p.TextWidth(t, d * 0.60f, Weight::Normal);
    bool hasFont = tw >= 0;
    if (!hasFont) tw = d * 0.5f;
    float w = std::max(d, tw + d * 0.55f);
    float bx = cx + body / 2 + body * 0.13f - w / 2, by = cy - body / 2 - body * 0.10f + d / 2;  // past the right edge and the top
    Rect r{ bx - w / 2, by - d / 2, bx + w / 2, by + d / 2 };
    p.FillRoundedRect(Rect{ r.left, r.top + d * 0.04f, r.right, r.bottom + d * 0.06f }, d / 2, Color{ 0, 0, 0, 0.18f });  // small shadow
    p.FillRoundedRect(r, d / 2, Color{ 1.0f, 0.231f, 0.188f, 1 });  // the system red (#FF3B30)
    if (hasFont) p.Text(t, Rect{ r.left, r.top - d * 0.04f, r.right, r.bottom - d * 0.04f }, d * 0.60f, Weight::Normal, Align::Center, Color{ 1, 1, 1, 1 });
}
