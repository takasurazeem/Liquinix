// Apps' notification counts on Linux (#81): the com.canonical.Unity.LauncherEntry D-Bus API, which Telegram,
// Thunderbird, Discord, Signal and others use and Plasma's task manager shows. Apps send
//   Update(app_uri "application://<desktop id>.desktop", { "count": int64, "count-visible": bool, ... })
// as a signal on the session bus; each signal may carry only some of the properties. Event-driven, no polling.
#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

class LauncherEntries : public QObject {
    Q_OBJECT
public:
    explicit LauncherEntries(QObject* parent = nullptr);
    bool Connected() const { return connected_; }
    // the badge's number for a .desktop id (ignoring case); 0 = no badge
    int Count(const QString& desktopId) const;

signals:
    void Changed();

private slots:
    void OnUpdate(const QString& appUri, const QVariantMap& properties);

private:
    struct State { qint64 count = 0; bool visible = false; };
    QHash<QString, State> states_;  // by lower-case .desktop id
    bool connected_ = false;
};
