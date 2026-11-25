// Apps do Dock no Linux (ver apps.h).
#include "apps.h"
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

namespace {

// [Desktop Entry] de um .desktop: chave -> valor (com as variantes de língua: Name[pt_BR]...)
QMap<QString, QString> LerEntrada(const QString& arquivo) {
    QMap<QString, QString> m;
    QFile f(arquivo);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    QTextStream in(&f);
    bool dentro = false;
    while (!in.atEnd()) {
        QString l = in.readLine().trimmed();
        if (l.startsWith('[')) { dentro = l == QLatin1String("[Desktop Entry]"); continue; }
        if (!dentro || l.isEmpty() || l.startsWith('#')) continue;
        int eq = l.indexOf('=');
        if (eq > 0) m.insert(l.left(eq).trimmed(), l.mid(eq + 1).trimmed());
    }
    return m;
}

// Name na língua do sistema: Name[pt_BR], Name[pt], senão Name
QString Localizado(const QMap<QString, QString>& m, const QString& chave) {
    QString loc = QLocale::system().name();  // "pt_BR"
    for (const QString& k : { chave + '[' + loc + ']', chave + '[' + loc.section('_', 0, 0) + ']' })
        if (m.contains(k)) return m.value(k);
    return m.value(chave);
}

// tira os códigos de campo do Exec (%f %F %u %U %i %c %k...); %% vira %
QString SemCodigos(const QString& exec) {
    QString r;
    for (int i = 0; i < exec.size(); i++) {
        if (exec[i] == '%' && i + 1 < exec.size()) { if (exec[i + 1] == '%') r += '%'; i++; continue; }
        r += exec[i];
    }
    return r.simplified();
}

}  // namespace

std::optional<AppDesktop> AcharApp(const QString& id) {
    QString arquivo = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id + QStringLiteral(".desktop"));
    if (arquivo.isEmpty()) return std::nullopt;
    auto m = LerEntrada(arquivo);
    if (m.value(QStringLiteral("Type")) != QLatin1String("Application")) return std::nullopt;
    if (m.value(QStringLiteral("NoDisplay")) == QLatin1String("true") || m.value(QStringLiteral("Hidden")) == QLatin1String("true")) return std::nullopt;
    AppDesktop a;
    a.id = id; a.arquivo = arquivo;
    a.nome = Localizado(m, QStringLiteral("Name"));
    a.icone = m.value(QStringLiteral("Icon"));
    a.exec = SemCodigos(m.value(QStringLiteral("Exec")));
    if (a.exec.isEmpty()) return std::nullopt;
    return a;
}

std::vector<AppDesktop> LerFixos(const QString& pasta) {
    QString arq = pasta + QStringLiteral("/dock.txt");
    QStringList ids;
    QFile f(arq);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) { QString l = in.readLine().trimmed(); if (!l.isEmpty() && !l.startsWith('#')) ids << l; }
    } else {
        // primeira vez: os apps comuns que existem aqui (KDE primeiro, depois alternativas)
        const QStringList comuns = { "org.kde.dolphin", "org.mozilla.firefox", "firefox", "org.kde.konsole", "org.kde.kate",
                                     "org.kde.discover", "systemsettings", "org.kde.systemsettings" };
        for (const QString& id : comuns) if (AcharApp(id)) ids << id;
        QDir().mkpath(pasta);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "# Apps fixos do Dock do Liquinix: um id de .desktop por linha (o nome do arquivo sem .desktop)\n";
            for (const QString& id : ids) out << id << '\n';
        }
    }
    std::vector<AppDesktop> r;
    for (const QString& id : ids) if (auto a = AcharApp(id)) r.push_back(*a);
    return r;
}

bool AbrirApp(const AppDesktop& app) {
    QStringList partes = QProcess::splitCommand(app.exec);
    if (partes.isEmpty()) return false;
    QString programa = partes.takeFirst();
    return QProcess::startDetached(programa, partes);
}
