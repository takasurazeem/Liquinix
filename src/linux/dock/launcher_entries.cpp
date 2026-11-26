// Apps' notification counts (see launcher_entries.h).
#include "launcher_entries.h"
#include <QDBusConnection>
#include <algorithm>
#include <cstdio>

LauncherEntries::LauncherEntries(QObject* parent) : QObject(parent) {
    // any sender and object path: apps choose their own
    connected_ = QDBusConnection::sessionBus().connect(QString(), QString(), QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                       QStringLiteral("Update"), this, SLOT(OnUpdate(QString, QVariantMap)));
}

int LauncherEntries::Count(const QString& desktopId) const {
    auto it = states_.constFind(desktopId.toLower());
    if (it == states_.constEnd() || !it->visible || it->count <= 0) return 0;
    return int(std::min<qint64>(it->count, 1000000));
}

void LauncherEntries::OnUpdate(const QString& appUri, const QVariantMap& properties) {
    // "application://org.telegram.desktop.desktop" -> "org.telegram.desktop"; some apps leave out the scheme
    QString id = appUri;
    if (id.startsWith(QLatin1String("application://"))) id = id.mid(14);
    if (id.endsWith(QLatin1String(".desktop"))) id.chop(8);
    if (id.isEmpty()) return;
    State& s = states_[id.toLower()];
    State before = s;
    if (properties.contains(QStringLiteral("count"))) s.count = properties.value(QStringLiteral("count")).toLongLong();
    if (properties.contains(QStringLiteral("count-visible"))) s.visible = properties.value(QStringLiteral("count-visible")).toBool();
    if (s.count == before.count && s.visible == before.visible) return;
    std::printf("liquinix-dock: badge %s: %lld%s\n", qPrintable(id), static_cast<long long>(s.count), s.visible ? "" : " (hidden)");
    std::fflush(stdout);
    emit Changed();
}
