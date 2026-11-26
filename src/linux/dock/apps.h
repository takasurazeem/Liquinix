// The Dock's apps on Linux: the pinned ones (dock.txt in the settings folder, one .desktop id per line) and how to
// launch each. .desktop files come from the XDG folders (Desktop Entry Specification): $XDG_DATA_HOME/applications and
// $XDG_DATA_DIRS/applications.
#pragma once
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

// an app's own action from its .desktop file ([Desktop Action new-window]: "New Window"), like a jump-list task
struct DesktopAction {
    QString name;  // Name, in the system's language if the file has it
    QString exec;  // Exec, without the field codes
};

struct DesktopApp {
    QString id;       // "org.kde.dolphin" (the file name without .desktop)
    QString name;     // Name (in the system's language, if the file has it)
    QString icon;     // Icon: a name in the icon theme or an absolute path
    QString exec;     // Exec, without the field codes (%f, %U...)
    QString file;     // path of the .desktop file
    QString wmClass;  // StartupWMClass: the windows' app_id when it isn't the .desktop id
    std::vector<DesktopAction> actions;  // Actions=, in the file's order
};

// finds the .desktop file by id; empty if it doesn't exist, is hidden (NoDisplay/Hidden) or isn't an application
std::optional<DesktopApp> FindApp(const QString& id);

// the pinned apps: reads <dir>/dock.txt; if it doesn't exist, writes a starting list with the common apps found here
std::vector<DesktopApp> ReadPinned(const QString& dir);

// saves the pinned apps, in this order, to <dir>/dock.txt (replacing it); false if it couldn't
bool WritePinned(const QString& dir, const std::vector<DesktopApp>& apps);

// starts the app (a separate process, without waiting); false if it couldn't
bool LaunchApp(const DesktopApp& app);
// runs a command line from a .desktop file (an action's Exec), the same way
bool LaunchCommand(const QString& exec);

// Open at Login: whether an autostart entry for the app is active ($XDG_CONFIG_HOME/autostart/<id>.desktop, or a
// system-wide one in $XDG_CONFIG_DIRS/autostart), and turning it on (a copy of the app's .desktop file) or off (removing
// the copy, and hiding a system-wide entry with Hidden=true). False if it couldn't.
bool OpensAtLogin(const DesktopApp& app);
bool SetOpenAtLogin(const DesktopApp& app, bool on);

// Does a window (by its Wayland app_id) belong to this app? Compares, ignoring case, with the .desktop id, its last
// part (org.kde.dolphin -> dolphin) and StartupWMClass.
bool WindowBelongsTo(const DesktopApp& app, const QString& appId);
