// Parts of the Dock's drawing that are the same on every platform (they draw through drawing::Painter).
#pragma once
#include "drawing.h"

namespace dock_drawing {

// The red badge with an app's notification count, at the top-right corner of `icon` (the icon's body, in points).
// n > 99 shows as "99+".
void Badge(drawing::Painter& p, const drawing::Rect& icon, int n);

}  // namespace dock_drawing
