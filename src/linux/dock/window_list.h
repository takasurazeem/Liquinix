// Open windows, from Plasma's window-management protocol (org_kde_plasma_window_management, the one Plasma's own
// taskbar uses). KWin may restrict it to trusted programs (a .desktop file listing
// X-KDE-Wayland-Interfaces=org_kde_plasma_window_management); the nested KWin of docs/linux-dev.md offered it without
// one. When it isn't offered, Available() stays false and the Dock works without the "running" dots.
#pragma once
#include <QString>
#include <QStringList>
#include <QWaylandClientExtensionTemplate>
#include <functional>
#include <memory>
#include <vector>
#include "qwayland-plasma-window-management.h"

class PlasmaWindow;

class WindowList : public QWaylandClientExtensionTemplate<WindowList>, public QtWayland::org_kde_plasma_window_management {
    Q_OBJECT
public:
    WindowList();
    ~WindowList() override;
    bool Available() const { return isActive(); }
    // app_id of the windows that show in the taskbar (skip-taskbar ones left out), oldest first
    QStringList AppIds() const;
    // brings the most recent window whose app_id `belongs` accepts to the front (and out of minimized)
    bool Activate(const std::function<bool(const QString&)>& belongs);
    // asks every window whose app_id `belongs` accepts to close (the app may still ask to save); returns how many
    int CloseAll(const std::function<bool(const QString&)>& belongs);

signals:
    void Changed();  // a window opened or closed, or changed its app_id or state

protected:
    void org_kde_plasma_window_management_window_with_uuid(uint32_t id, const QString& uuid) override;

private:
    friend class PlasmaWindow;
    void Gone(PlasmaWindow* w);
    std::vector<std::unique_ptr<PlasmaWindow>> windows_;
};

class PlasmaWindow : public QtWayland::org_kde_plasma_window {
public:
    PlasmaWindow(WindowList* owner, struct ::org_kde_plasma_window* obj) : QtWayland::org_kde_plasma_window(obj), owner_(owner) {}
    ~PlasmaWindow() override { if (object()) destroy(); }
    QString appId;
    uint32_t state = 0;
    bool ready = false;  // initial_state has arrived (before that, app_id and state may be incomplete)

protected:
    void org_kde_plasma_window_app_id_changed(const QString& id) override { appId = id; if (ready) emit owner_->Changed(); }
    void org_kde_plasma_window_state_changed(uint32_t flags) override { state = flags; if (ready) emit owner_->Changed(); }
    void org_kde_plasma_window_initial_state() override { ready = true; emit owner_->Changed(); }
    void org_kde_plasma_window_unmapped() override { owner_->Gone(this); }

private:
    WindowList* owner_;
};
