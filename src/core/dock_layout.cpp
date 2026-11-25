// The Dock's layout (see dock_layout.h). Moved from Layout() in src/shell/dock.cpp with the same arithmetic, in the
// same order: on Windows the result is identical, number for number.
#include "dock_layout.h"
#include "curves.h"
#include <algorithm>
#include <cmath>

dock_layout::Result dock_layout::Compute(std::vector<Item>& items, const Params& p) {
    Result res;
    float W = p.axis;
    float slot = p.step;
    // base width, to find the centres without magnification
    float base = p.margin;
    std::vector<float> c0;
    for (auto& it : items) { float w = it.separator ? p.separatorWidth : slot; c0.push_back(base + w / 2); base += w; }
    base += p.margin;
    float left0 = (W - base) / 2;
    // Tahoe (guide, section 63): a cos² curve reaching ~3.5 icons to each side (measured 85/82/74/65 with a base of 64)
    float M = p.magnified / p.icon, R = p.tahoe ? 3.5f : 2.6f;
    std::vector<float> wv;
    float total = p.margin * 2;
    for (size_t i = 0; i < items.size(); i++) {
        float s = 1;
        float mx = p.mouse >= 0 ? p.mouse : p.lastMouse;
        if (p.magnify && mx >= 0 && p.magnification > 0.001f && !items[i].separator && !p.dragging) {
            s = curves::DockMagnify(std::fabs(mx - (left0 + c0[i])) / slot, R, M, p.magnification);
        }
        float sepS = 1;  // Tahoe (guide, section 63): the separator also grows near the pointer (31 -> 40 with 64)
        if (p.tahoe && p.magnify && mx >= 0 && p.magnification > 0.001f && items[i].separator && !p.dragging) {
            sepS = curves::DockMagnify(std::fabs(mx - (left0 + c0[i])) / slot, R, 40.f / 31.f, p.magnification);
        }
        items[i].size = items[i].separator ? 0 : p.icon * s;
        wv.push_back(items[i].separator ? p.separatorWidth * sepS : slot * s * items[i].factor);
        total += wv.back();
    }
    // Dragging a pinned app: the others open a gap live where it would land (as on the Mac); dragged out of the Dock,
    // its place closes. dropTarget holds the target position (an index in the app list).
    std::vector<int> order;
    for (int i = 0; i < (int)items.size(); i++) order.push_back(i);
    int drag = p.dragging && p.dragged >= 0 && p.dragged < (int)items.size() && items[p.dragged].app ? p.dragged : -1;
    if (drag >= 0) {
        order.erase(std::find(order.begin(), order.end(), drag));
        total -= wv[drag];
        if (!p.outsideDock) {
            // centres without the dragged one; the new place is among the pinned apps, after the Finder and Launchpad
            float xx = (W - total - wv[drag]) / 2 + p.margin;
            int pos = -1, firstPinned = -1, lastPinned = -1;
            for (int k = 0; k < (int)order.size(); k++) {
                auto& it = items[order[k]];
                bool pinned = it.app && it.movablePinned;
                if (pinned) { if (firstPinned < 0) firstPinned = k; lastPinned = k; }
                float c = xx + wv[order[k]] / 2;
                if (pos < 0 && pinned && p.mouse < c) pos = k;
                xx += wv[order[k]];
            }
            if (firstPinned < 0) firstPinned = lastPinned = (int)order.size() - 1;
            if (pos < 0) pos = lastPinned + 1;
            pos = std::clamp(pos, firstPinned, lastPinned + 1);
            order.insert(order.begin() + pos, drag);
            total += wv[drag];
            int from = items[drag].appIndex;
            if (pos > firstPinned) { int prev = items[order[pos - 1]].appIndex; res.dropTarget = prev + (prev < from ? 1 : 0); }
            else if (pos + 1 < (int)order.size() && items[order[pos + 1]].app) res.dropTarget = items[order[pos + 1]].appIndex - (items[order[pos + 1]].appIndex > from ? 1 : 0);
            else res.dropTarget = from;
        }
    }
    float x = (W - total) / 2 + p.margin;
    for (int i : order) {
        float target = x + wv[i] / 2;
        // slide to the new place instead of jumping (only while dragging; the frame clock is on)
        items[i].cx = p.dragging && items[i].cx > 0 ? items[i].cx + (target - items[i].cx) * 0.35f : target;
        x += wv[i];
    }
    if (drag >= 0 && p.outsideDock) items[drag].cx = -1000;
    res.total = total;
    return res;
}
