// The current virtual desktop, from Plasma's org_kde_plasma_virtual_desktop_management protocol (the one Plasma's
// pager uses), for the Dock's smart hiding: only windows on the current desktop can cover the Dock. When the protocol
// isn't offered, Current() stays empty and every window counts.
#pragma once
#include <QString>
#include <QWaylandClientExtensionTemplate>
#include <memory>
#include <vector>
#include "qwayland-org-kde-plasma-virtual-desktop.h"

class PlasmaDesktop;

class VirtualDesktops : public QWaylandClientExtensionTemplate<VirtualDesktops>, public QtWayland::org_kde_plasma_virtual_desktop_management {
    Q_OBJECT
public:
    VirtualDesktops();
    ~VirtualDesktops() override;
    QString Current() const { return current_; }

signals:
    void CurrentChanged();

protected:
    void org_kde_plasma_virtual_desktop_management_desktop_created(const QString& id, uint32_t position) override;
    void org_kde_plasma_virtual_desktop_management_desktop_removed(const QString& id) override;

private:
    friend class PlasmaDesktop;
    void Activated(const QString& id) { if (id != current_) { current_ = id; emit CurrentChanged(); } }
    std::vector<std::unique_ptr<PlasmaDesktop>> desktops_;
    QString current_;
};

class PlasmaDesktop : public QtWayland::org_kde_plasma_virtual_desktop {
public:
    PlasmaDesktop(VirtualDesktops* owner, QString id, struct ::org_kde_plasma_virtual_desktop* obj)
        : QtWayland::org_kde_plasma_virtual_desktop(obj), id(std::move(id)), owner_(owner) {}
    ~PlasmaDesktop() override { if (object()) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(object())); }  // no destructor request
    QString id;

protected:
    void org_kde_plasma_virtual_desktop_activated() override { owner_->Activated(id); }

private:
    VirtualDesktops* owner_;
};
