// Apps do Dock no Linux: os fixos (dock.txt na pasta dos ajustes, um id de .desktop por linha) e como abrir cada um.
// Os .desktop vêm das pastas do padrão XDG (Desktop Entry Specification): $XDG_DATA_HOME/applications e
// $XDG_DATA_DIRS/applications.
#pragma once
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

struct AppDesktop {
    QString id;       // "org.kde.dolphin" (o nome do arquivo sem .desktop)
    QString nome;     // Name (na língua do sistema, se o arquivo tiver)
    QString icone;    // Icon: nome no tema de ícones ou caminho absoluto
    QString exec;     // Exec, sem os códigos de campo (%f, %U...)
    QString arquivo;  // caminho do .desktop
};

// procura o .desktop pelo id; vazio se não existir, se estiver escondido (NoDisplay/Hidden) ou não for um app
std::optional<AppDesktop> AcharApp(const QString& id);

// os fixos: lê <pasta>/dock.txt; se não existir, grava uma lista inicial com os apps comuns que existem neste sistema
std::vector<AppDesktop> LerFixos(const QString& pasta);

// abre o app (processo à parte, sem esperar); false se não deu
bool AbrirApp(const AppDesktop& app);
