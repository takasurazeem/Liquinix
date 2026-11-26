// Open windows from Plasma's protocol (see window_list.h).
#include "window_list.h"
#include <algorithm>
#include <wayland-client-core.h>

// version 16: window_with_uuid (13) and initial_state (4); Qt binds the lower of ours and KWin's
WindowList::WindowList() : QWaylandClientExtensionTemplate<WindowList>(16) { initialize(); }

WindowList::~WindowList() {
    windows_.clear();
    // the manager has no destructor request in the protocol, and Qt's wrapper doesn't free it: release our proxy
    if (object()) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(object()));
}

void WindowList::org_kde_plasma_window_management_window_with_uuid(uint32_t, const QString& uuid) {
    windows_.push_back(std::make_unique<PlasmaWindow>(this, get_window_by_uuid(uuid)));
}

void WindowList::Gone(PlasmaWindow* w) {
    auto it = std::find_if(windows_.begin(), windows_.end(), [&](auto& p) { return p.get() == w; });
    if (it == windows_.end()) return;
    windows_.erase(it);  // the destructor sends destroy
    emit Changed();
}

QStringList WindowList::AppIds() const {
    QStringList r;
    for (auto& w : windows_)
        if (w->ready && !w->appId.isEmpty() && !(w->state & org_kde_plasma_window_management::state_skiptaskbar)) r << w->appId;
    return r;
}

bool WindowList::Activate(const std::function<bool(const QString&)>& belongs) {
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
        PlasmaWindow& w = **it;
        if (!w.ready || !belongs(w.appId) || (w.state & org_kde_plasma_window_management::state_skiptaskbar)) continue;
        // activate (and unminimize): the same request Plasma's taskbar sends
        w.set_state(org_kde_plasma_window_management::state_active | org_kde_plasma_window_management::state_minimized,
                    org_kde_plasma_window_management::state_active);
        return true;
    }
    return false;
}

int WindowList::CloseAll(const std::function<bool(const QString&)>& belongs) {
    int n = 0;
    for (auto& w : windows_)
        if (w->ready && belongs(w->appId) && !(w->state & org_kde_plasma_window_management::state_skiptaskbar)) { w->close(); n++; }
    return n;
}
