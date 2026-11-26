// The current virtual desktop (see virtual_desktops.h).
#include "virtual_desktops.h"
#include <wayland-client-core.h>
#include <algorithm>

VirtualDesktops::VirtualDesktops() : QWaylandClientExtensionTemplate<VirtualDesktops>(2) { initialize(); }

VirtualDesktops::~VirtualDesktops() {
    desktops_.clear();
    // the manager has no destructor request in the protocol, and Qt's wrapper doesn't free it: release our proxy
    if (object()) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(object()));
}

void VirtualDesktops::org_kde_plasma_virtual_desktop_management_desktop_created(const QString& id, uint32_t) {
    desktops_.push_back(std::make_unique<PlasmaDesktop>(this, id, get_virtual_desktop(id)));
}

void VirtualDesktops::org_kde_plasma_virtual_desktop_management_desktop_removed(const QString& id) {
    desktops_.erase(std::remove_if(desktops_.begin(), desktops_.end(), [&](auto& d) { return d->id == id; }), desktops_.end());
}
