// The Dock's apps on Linux (see apps.h).
#include "apps.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {

// a .desktop file's groups ("Desktop Entry", "Desktop Action new-window"...): key -> value (including the language
// variants: Name[pt_BR]...)
QMap<QString, QMap<QString, QString>> ReadGroups(const QString& file) {
    QMap<QString, QMap<QString, QString>> groups;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return groups;
    QTextStream in(&f);
    QString group;
    while (!in.atEnd()) {
        QString l = in.readLine().trimmed();
        if (l.startsWith('[') && l.endsWith(']')) { group = l.mid(1, l.size() - 2); continue; }
        if (group.isEmpty() || l.isEmpty() || l.startsWith('#')) continue;
        int eq = l.indexOf('=');
        if (eq > 0) groups[group].insert(l.left(eq).trimmed(), l.mid(eq + 1).trimmed());
    }
    return groups;
}
QMap<QString, QString> ReadEntry(const QString& file) { return ReadGroups(file).value(QStringLiteral("Desktop Entry")); }

// a key in the system's language: Name[pt_BR], Name[pt], else Name
QString Localized(const QMap<QString, QString>& m, const QString& key) {
    QString loc = QLocale::system().name();  // "pt_BR"
    for (const QString& k : { key + '[' + loc + ']', key + '[' + loc.section('_', 0, 0) + ']' })
        if (m.contains(k)) return m.value(k);
    return m.value(key);
}

// removes the field codes from Exec (%f %F %u %U %i %c %k...); %% becomes %
QString StripFieldCodes(const QString& exec) {
    QString r;
    for (int i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) { if (exec[i + 1] == '%') r += '%'; i++; continue; }
        r += exec[i];
    }
    return r.simplified();
}

}  // namespace

std::optional<DesktopApp> FindApp(const QString& id) {
    QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id + QStringLiteral(".desktop"));
    if (file.isEmpty()) return std::nullopt;
    auto groups = ReadGroups(file);
    auto m = groups.value(QStringLiteral("Desktop Entry"));
    if (m.value(QStringLiteral("Type")) != QLatin1String("Application")) return std::nullopt;
    if (m.value(QStringLiteral("NoDisplay")) == QLatin1String("true") || m.value(QStringLiteral("Hidden")) == QLatin1String("true")) return std::nullopt;
    DesktopApp a;
    a.id = id; a.file = file;
    a.name = Localized(m, QStringLiteral("Name"));
    a.icon = m.value(QStringLiteral("Icon"));
    a.exec = StripFieldCodes(m.value(QStringLiteral("Exec")));
    a.wmClass = m.value(QStringLiteral("StartupWMClass"));
    if (a.exec.isEmpty()) return std::nullopt;
    for (const QString& action : m.value(QStringLiteral("Actions")).split(';', Qt::SkipEmptyParts)) {
        auto g = groups.value(QStringLiteral("Desktop Action ") + action.trimmed());
        DesktopAction da{ Localized(g, QStringLiteral("Name")), StripFieldCodes(g.value(QStringLiteral("Exec"))) };
        if (!da.name.isEmpty() && !da.exec.isEmpty()) a.actions.push_back(da);
    }
    return a;
}

std::vector<DesktopApp> ReadPinned(const QString& dir) {
    QString path = dir + QStringLiteral("/dock.txt");
    QStringList ids;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) { QString l = in.readLine().trimmed(); if (!l.isEmpty() && !l.startsWith('#')) ids << l; }
    } else {
        // first run: the common apps that exist here (KDE first, then alternatives)
        const QStringList common = { "org.kde.dolphin", "org.mozilla.firefox", "firefox", "org.kde.konsole", "org.kde.kate",
                                     "org.kde.discover", "systemsettings", "org.kde.systemsettings" };
        for (const QString& id : common) if (FindApp(id)) ids << id;
        QDir().mkpath(dir);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "# Liquinix Dock pinned apps: one .desktop id per line (the file name without .desktop)\n";
            for (const QString& id : ids) out << id << '\n';
        }
    }
    std::vector<DesktopApp> r;
    for (const QString& id : ids) if (auto a = FindApp(id)) r.push_back(*a);
    return r;
}

bool WritePinned(const QString& dir, const std::vector<DesktopApp>& apps) {
    QDir().mkpath(dir);
    QSaveFile f(dir + QStringLiteral("/dock.txt"));  // written to a temporary file and renamed: never half a list
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out << "# Liquinix Dock pinned apps: one .desktop id per line (the file name without .desktop)\n";
    for (const DesktopApp& a : apps) out << a.id << '\n';
    out.flush();
    return f.commit();
}

bool LaunchApp(const DesktopApp& app) { return LaunchCommand(app.exec); }

bool LaunchCommand(const QString& exec) {
    QStringList parts = QProcess::splitCommand(exec);
    if (parts.isEmpty()) return false;
    QString program = parts.takeFirst();
    return QProcess::startDetached(program, parts);
}

bool WindowBelongsTo(const DesktopApp& app, const QString& appId) {
    if (appId.isEmpty()) return false;
    auto same = [&](const QString& x) { return !x.isEmpty() && x.compare(appId, Qt::CaseInsensitive) == 0; };
    return same(app.id) || same(app.wmClass) || same(app.id.section('.', -1));
}

namespace {
QString UserAutostartFile(const DesktopApp& app) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/") + app.id + QStringLiteral(".desktop");
}
// an autostart entry counts unless it's hidden or turned off (GNOME's key, which Plasma honours too)
bool EntryActive(const QString& file) {
    auto m = ReadEntry(file);
    return m.value(QStringLiteral("Hidden")) != QLatin1String("true") && m.value(QStringLiteral("X-GNOME-Autostart-enabled")) != QLatin1String("false");
}
}  // namespace

bool OpensAtLogin(const DesktopApp& app) {
    // the user's own entry wins over a system-wide one of the same name (XDG Autostart specification)
    QString file = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("autostart/") + app.id + QStringLiteral(".desktop"));
    return !file.isEmpty() && EntryActive(file);
}

bool SetOpenAtLogin(const DesktopApp& app, bool on) {
    if (app.file.isEmpty()) return false;  // no .desktop file to start it from
    QString mine = UserAutostartFile(app);
    QDir().mkpath(QFileInfo(mine).path());
    QFile::remove(mine);
    if (on) return QFile::copy(app.file, mine);
    // off: if a system-wide entry is still found, hide it with an entry of our own
    QString system = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("autostart/") + app.id + QStringLiteral(".desktop"));
    if (system.isEmpty() || !EntryActive(system)) return true;
    QSaveFile f(mine);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&f);
    out << "[Desktop Entry]\nType=Application\nName=" << app.name << "\nExec=" << app.exec << "\nHidden=true\n";
    out.flush();
    return f.commit();
}
