// The Dock's layout along its edge, the same on every platform (#19): each item's centre, each icon's size with
// magnification, the gap that opens while dragging, and the background's length.
// All in points, along the Dock's axis (standing upright on the sides, the same arithmetic).
#pragma once
#include <vector>

namespace dock_layout {

struct Item {
    bool separator = false;
    bool app = false;            // an app (pinned ones can be dragged to a new place)
    bool movablePinned = false;  // a pinned app that can move (pinned and not the Finder)
    int appIndex = -1;           // the app's position in the app list (to say where the dragged one lands)
    float factor = 1;            // 0..1: how much of its space the item takes (entering or leaving the Dock gradually)
    float cx = 0;                // in: the current centre (dragging slides from it); out: the new centre
    float size = 0;              // out: the icon's size (0 for the separator)
};

struct Params {
    float axis = 0;              // length available along the Dock
    float icon = 48;             // normal icon size
    float magnified = 48;        // largest size with magnification
    float step = 0;              // the space of one icon (icon + gap)
    float margin = 0;            // margin before the first and after the last item
    float separatorWidth = 0;
    bool magnify = false;        // magnification turned on in the settings
    bool tahoe = false;          // macOS 26 measurements: a longer reach, and the separator grows too
    float mouse = -1;            // the pointer along the Dock; -1 = outside
    float lastMouse = -1;        // the last position inside (magnification shrinks back from there when leaving)
    float magnification = 0;     // how much of the magnification is applied (0..1, animates when entering and leaving)
    bool dragging = false;
    int dragged = -1;            // index (in items) of the pinned app being dragged; -1 = none
    bool outsideDock = false;    // the dragged app left the Dock: its place closes
};

struct Result {
    float total = 0;             // the background's length
    int dropTarget = -1;         // while dragging: the target position in the app list; -1 = none
};

Result Compute(std::vector<Item>& items, const Params& p);

}  // namespace dock_layout
