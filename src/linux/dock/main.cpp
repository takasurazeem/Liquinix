// liquinix-dock: o Dock do Liquinix no Linux (Qt 6, layer-shell; ver docs/linux-roadmap.md).
// Por enquanto (#18) só o esqueleto: uma superfície de layer-shell presa à borda de baixo, desenhada com QPainter.
// Os ícones, os apps abertos e a ampliação vêm no #20, com a lógica compartilhada de src/nucleo.
#include "nucleo/curvas.h"
#include "plataforma/plataforma.h"

#include <LayerShellQt/Window>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QRasterWindow>
#include <QSurfaceFormat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const int W = 520, H = 84;  // pt: o fundo do Dock e a margem até a borda da tela

class DockWindow : public QRasterWindow {
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // o fundo: a "prateleira" arredondada, como o fundo do Dock no Windows (sem o vidro por enquanto)
        QRectF bg(8, 8, width() - 16, height() - 16);
        QPainterPath path; path.addRoundedRect(bg, 22, 22);
        p.fillPath(path, QColor(245, 245, 247, 200));
        p.setPen(QColor(255, 255, 255, 180)); p.drawPath(path);
        p.setPen(QColor(30, 30, 30));
        p.drawText(bg, Qt::AlignCenter, QStringLiteral("liquinix-dock"));
    }
};

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("liquinix-dock"));
    const char* safe = std::getenv("LIQUINIX_SAFE_MODE");
    bool safeMode = safe && std::strcmp(safe, "1") == 0;  // modo seguro: nada opcional (ainda não há nada a desligar)
    std::printf("liquinix-dock: %s; ajustes em %ls; Ease(0,5) = %.3f\n", safeMode ? "modo seguro" : "normal",
                plataforma::PastaAjustes().c_str(), curvas::Ease(0.5f));
    std::fflush(stdout);  // a saída vai para um arquivo ou cano: sem isto a linha só sairia ao fechar

    DockWindow w;
    QSurfaceFormat fmt = w.format(); fmt.setAlphaBufferSize(8); w.setFormat(fmt);  // fundo transparente
    w.resize(W, H);
    if (auto* ls = LayerShellQt::Window::get(&w)) {  // antes de mostrar: vira uma superfície de layer-shell
        ls->setScope(QStringLiteral("liquinix-dock"));
        ls->setLayer(LayerShellQt::Window::LayerTop);
        ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorBottom));
        ls->setExclusiveZone(0);  // por cima das janelas, sem reservar espaço (como o Dock do Mac)
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    }
    w.show();
    return app.exec();
}
