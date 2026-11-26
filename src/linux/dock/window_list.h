// Open windows, from Plasma's window-management protocol (org_kde_plasma_window_management, the one Plasma's own
// taskbar uses). KWin may restrict it to trusted programs (a .desktop file listing
// X-KDE-Wayland-Interfaces=org_kde_plasma_window_management); the nested KWin of docs/linux-dev.md offered it without
// one. When it isn't offered, Available() stays false and the Dock works without the "running" dots.
#pragma once
#include <QRect>
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
    // the windows whose app_id `belongs` accepts (taskbar ones), oldest first: their titles and a key for ActivateWindow
    struct Info { QString title; quintptr key; };
    std::vector<Info> Windows(const std::function<bool(const QString&)>& belongs) const;
    // every taskbar window, oldest first (for the Dock's minimized windows)
    struct Window { QString appId, title; quintptr key; bool minimized; };
    std::vector<Window> All() const;
    // brings one window (a key from Windows()) to the front; false if it has closed since
    bool ActivateWindow(quintptr key);
    // Smart hiding: does a window cover `area` (compositor coordinates)? Only taskbar windows that aren't minimized, on
    // `desktop` (or on all desktops; an empty `desktop` counts every window), whose app_id `ignore` doesn't reject.
    bool Covers(const QRect& area, const QString& desktop, const std::function<bool(const QString&)>& ignore) const;
    // minimizes every window whose app_id `belongs` accepts (Hide); returns how many
    int MinimizeAll(const std::function<bool(const QString&)>& belongs);

signals:
    void Changed();  // a window opened or closed, or changed its app_id or state
    void Moved();    // a window's geometry or virtual desktops changed (smart hiding only)

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
    QString title;
    uint32_t state = 0;
    QRect geometry;        // in compositor coordinates (geometry event, protocol version 6)
    QStringList desktops;  // the virtual desktops it's on (ids)
    bool ready = false;  // initial_state has arrived (before that, app_id and state may be incomplete)

protected:
    void org_kde_plasma_window_app_id_changed(const QString& id) override { appId = id; if (ready) emit owner_->Changed(); }
    void org_kde_plasma_window_title_changed(const QString& t) override { title = t; }  // read when a menu opens: no signal
    void org_kde_plasma_window_geometry(int32_t x, int32_t y, uint32_t w, uint32_t h) override {
        geometry = QRect(x, y, int(w), int(h)); if (ready) emit owner_->Moved();
    }
    void org_kde_plasma_window_virtual_desktop_entered(const QString& id) override { if (!desktops.contains(id)) desktops << id; if (ready) emit owner_->Moved(); }
    void org_kde_plasma_window_virtual_desktop_left(const QString& id) override { desktops.removeAll(id); if (ready) emit owner_->Moved(); }
    void org_kde_plasma_window_state_changed(uint32_t flags) override { state = flags; if (ready) emit owner_->Changed(); }
    void org_kde_plasma_window_initial_state() override { ready = true; emit owner_->Changed(); }
    void org_kde_plasma_window_unmapped() override { owner_->Gone(this); }

private:
    WindowList* owner_;
};
