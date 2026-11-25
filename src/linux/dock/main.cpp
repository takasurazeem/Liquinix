// liquinix-dock: o Dock do Liquinix no Linux (Qt 6, layer-shell; ver docs/linux-roadmap.md e a issue #20).
// Uma superfície de layer-shell presa à borda de baixo, do tamanho da largura da tela; o Dock desenhado no meio.
// A disposição (dock_layout), as curvas e o emblema são o mesmo código do Dock do Windows (src/nucleo); aqui fica só
// o que é do Linux: os apps (.desktop), os ícones do tema, abrir os apps, a janela e a entrada do mouse.
// Quadros só enquanto algo se mexe (update() -> requestUpdate, no ritmo do compositor); parado, nada roda.
#include "apps.h"
#include "nucleo/curvas.h"
#include "nucleo/dock_layout.h"
#include "plataforma/linux/pintor_qt.h"
#include "plataforma/plataforma.h"

#include <LayerShellQt/Window>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRasterWindow>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

// ajustes.txt (o mesmo formato do Windows: chave=valor por linha)
std::map<QString, QString> LerAjustes(const QString& pasta) {
    std::map<QString, QString> m;
    QFile f(pasta + QStringLiteral("/ajustes.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString l = in.readLine();
        int eq = l.indexOf('=');
        if (eq > 0) m[l.left(eq).trimmed()] = l.mid(eq + 1).trimmed();
    }
    return m;
}

class DockWindow : public QRasterWindow {
public:
    DockWindow(std::vector<AppDesktop> apps, float tile, float large, bool magnify)
        : apps_(std::move(apps)), tile_(tile), large_(large), magnify_(magnify) {
        for (auto& a : apps_) icons_.push_back(a.icone.startsWith('/') ? QIcon(a.icone) : QIcon::fromTheme(a.icone, QIcon::fromTheme(QStringLiteral("application-x-executable"))));
        setFlag(Qt::FramelessWindowHint);
    }
    // medidas do Tahoe (as mesmas do Dock do Windows, src/shell/dock.cpp)
    float PadX() const { return tile_ * 0.125f; }
    float Slot() const { return tile_ * 1.0625f; }
    float PadTop() const { return tile_ * 0.156f; }
    float PadBottom() const { return tile_ * 0.148f; }
    float Margin() const { return tile_ * 0.10f; }
    float Radius() const { return tile_ * 0.40f; }
    int AlturaJanela() const { return int(std::ceil(large_ + PadTop() + PadBottom() + Margin() + 52)); }

protected:
    void paintEvent(QPaintEvent*) override {
        double now = plataforma::AgoraMs();
        float dt = last_ > 0 ? float(std::clamp((now - last_) / 1000.0, 0.0, 0.05)) : 0.016f;
        last_ = now;
        // ampliação: chega ao alvo em ~0,15 s (exponencial por tempo, igual em 60 ou 144 Hz; a mesma conta do Windows)
        float alvo = mouseX_ >= 0 ? 1.f : 0.f;
        magAmt_ += (alvo - magAmt_) * (1 - std::exp(-dt / 0.05f));
        if (std::fabs(alvo - magAmt_) < 0.002f) magAmt_ = alvo;
        Layout(float(width()), float(height()));
        QPainter p(this);
        Desenhar(p, float(width()), float(height()));
        p.end();
        AtualizarMascara();
        bool anim = magAmt_ != alvo;
        if (anim) update(); else last_ = 0;  // próximo quadro só se ainda se mexe
    }

public:
    // LIQUINIX_DOCK_PNG: o mesmo desenho numa imagem (sem janela), com o ponteiro em `mouse` e a ampliação inteira
    bool SalvarPng(const QString& arquivo, int W, float mouse, qreal escala) {
        mouseX_ = lastMouseX_ = mouse; magAmt_ = mouse >= 0 ? 1.f : 0.f;
        int H = AlturaJanela();
        Layout(float(W), float(H));
        hover_ = -1;
        for (size_t i = 0; mouse >= 0 && i < itens_.size(); i++) if (std::fabs(mouse - itens_[i].cx) <= Slot() * itens_[i].tamanho / tile_ / 2) hover_ = int(i);
        QImage img(int(W * escala), int(H * escala), QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(escala);
        img.fill(QColor(70, 90, 120));  // um "papel de parede" liso para ver a transparência
        QPainter p(&img);
        Desenhar(p, float(W), float(H));
        p.end();
        return img.save(arquivo);
    }

protected:
    void Desenhar(QPainter& p, float W, float H) {
        (void)W; (void)H;
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (p.device() == this) {  // na janela: limpa para transparente (na imagem de teste fica o fundo dela)
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        PintorQt pq(p);
        // o fundo (sem o vidro por enquanto: o desfoque do KWin vem num passo seguinte)
        pq.RetArredondado(bg_, Radius(), desenho::Cor{ 0.96f, 0.96f, 0.97f, 0.78f });
        QPainterPath borda; borda.addRoundedRect(QRectF(bg_.esq + 0.5, bg_.topo + 0.5, bg_.dir - bg_.esq - 1, bg_.base - bg_.topo - 1), Radius(), Radius());
        p.setPen(QPen(QColor(255, 255, 255, 170), 1)); p.drawPath(borda);
        float baseline = bg_.base - PadBottom();
        qreal dpr = p.device()->devicePixelRatio();
        for (size_t i = 0; i < itens_.size(); i++) {
            float s = itens_[i].tamanho, cx = itens_[i].cx;
            QRectF r(cx - s / 2, baseline - s, s, s);
            QPixmap px = icons_[i].pixmap(QSize(int(std::ceil(s)), int(std::ceil(s))), dpr);  // o QIcon guarda os tamanhos já feitos
            p.drawPixmap(r, px, QRectF(0, 0, px.width(), px.height()));
        }
        // o nome do app sob o ponteiro, numa etiqueta acima do ícone (como no Mac)
        if (hover_ >= 0 && magAmt_ > 0.5f) {
            float s = itens_[hover_].tamanho, cx = itens_[hover_].cx;
            std::wstring nome = apps_[hover_].nome.toStdWString();
            float tw = pq.LarguraTexto(nome, 13, desenho::Peso::Normal);
            if (tw >= 0) {
                float w = tw + 24, h = 26, top = baseline - s - 12 - h;
                desenho::Ret r{ cx - w / 2, top, cx + w / 2, top + h };
                pq.RetArredondado(r, 8, desenho::Cor{ 0.95f, 0.95f, 0.96f, 0.92f });
                pq.Texto(nome, r, 13, desenho::Peso::Normal, desenho::Alinhar::Centro, desenho::Cor{ 0.1f, 0.1f, 0.1f, 1 });
            }
        }
    }

    bool event(QEvent* e) override {
        if (e->type() == QEvent::Leave) { mouseX_ = -1; hover_ = -1; update(); }
        return QRasterWindow::event(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        float x = float(e->position().x()), y = float(e->position().y());
        bool dentro = x >= bg_.esq - large_ && x <= bg_.dir + large_ && y >= bg_.topo - (large_ - tile_) - 20;
        mouseX_ = dentro ? x : -1;
        if (dentro) lastMouseX_ = x;
        int h = -1;
        for (size_t i = 0; dentro && i < itens_.size(); i++) if (std::fabs(x - itens_[i].cx) <= Slot() * itens_[i].tamanho / tile_ / 2) h = int(i);
        hover_ = h;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || hover_ < 0) return;
        const AppDesktop& a = apps_[hover_];
        bool ok = AbrirApp(a);
        std::printf("liquinix-dock: abrir %s (%s): %s\n", qPrintable(a.id), qPrintable(a.exec), ok ? "ok" : "falhou");
        std::fflush(stdout);
    }
    void resizeEvent(QResizeEvent*) override { update(); }

private:
    void Layout(float W, float H) {
        itens_.assign(apps_.size(), {});
        for (size_t i = 0; i < apps_.size(); i++) { itens_[i].app = true; itens_[i].indiceApp = int(i); }
        dock_layout::Parametros prm;
        prm.eixo = W; prm.icone = tile_; prm.ampliado = large_; prm.passo = Slot(); prm.margem = PadX();
        prm.larguraSeparador = tile_ * 0.484f; prm.ampliar = magnify_; prm.tahoe = true;
        prm.mouse = mouseX_; prm.ultimoMouse = lastMouseX_; prm.ampliacao = magAmt_;
        auto res = dock_layout::Calcular(itens_, prm);
        float bottom = H - Margin();
        bg_ = desenho::Ret{ (W - res.total) / 2, bottom - (tile_ + PadTop() + PadBottom()), (W + res.total) / 2, bottom };
    }
    // só o Dock recebe o mouse; o resto da faixa deixa os cliques passarem para as janelas de baixo. Com o ponteiro
    // em cima, a área cresce para cima (os ícones ampliados) e para os lados.
    void AtualizarMascara() {
        QRect r = mouseX_ >= 0 ? QRectF(bg_.esq - large_, 0, bg_.dir - bg_.esq + 2 * large_, height()).toAlignedRect()
                               : QRectF(bg_.esq, bg_.topo, bg_.dir - bg_.esq, bg_.base - bg_.topo).toAlignedRect();
        if (r != mask_) { mask_ = r; setMask(QRegion(r)); }
    }

    std::vector<AppDesktop> apps_;
    std::vector<QIcon> icons_;
    std::vector<dock_layout::Item> itens_;
    float tile_, large_;
    bool magnify_;
    float mouseX_ = -1, lastMouseX_ = -1, magAmt_ = 0;
    int hover_ = -1;
    double last_ = 0;
    desenho::Ret bg_;
    QRect mask_;
};

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("liquinix-dock"));
    const char* safe = std::getenv("LIQUINIX_SAFE_MODE");
    bool safeMode = safe && std::strcmp(safe, "1") == 0;  // modo seguro: nada opcional (sem ampliação)
    QString pasta = QString::fromStdWString(plataforma::PastaAjustes());
    auto aj = LerAjustes(pasta);
    auto num = [&](const char* k, float def) { auto it = aj.find(QString::fromLatin1(k)); bool ok = false; float v = it == aj.end() ? def : it->second.toFloat(&ok); return ok ? v : def; };
    float tile = std::clamp(num("tamanho", 48), 16.f, 128.f);
    float large = std::clamp(num("ampliacao_tamanho", 128), tile, 256.f);
    bool magnify = !safeMode && aj["ampliacao"] == QLatin1String("1");
    // tema de ícones: o dos ajustes (tema_icones=MacTahoe, por exemplo), senão o do KDE (kdeglobals), senão o que o Qt
    // achou; sem nada disso (sem tema da plataforma, como no modo de imagem), o Breeze
    QString tema = aj["tema_icones"];
    if (tema.isEmpty()) {
        QString kg = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
        if (!kg.isEmpty()) tema = QSettings(kg, QSettings::IniFormat).value(QStringLiteral("Icons/Theme")).toString();
    }
    if (tema.isEmpty() && QIcon::themeName().isEmpty()) tema = QStringLiteral("breeze");
    // onde procurar os temas (Icon Theme Specification): <XDG_DATA_DIRS>/icons e ~/.icons. O tema da plataforma (KDE)
    // costuma pôr isto; sem ele o Qt só olha os ícones embutidos
    QStringList caminhos = QIcon::themeSearchPaths();
    QStringList pastasIcones;  // (numa variável: um "for" sobre o temporário de "lista << item" usaria memória já solta)
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) pastasIcones << d + QStringLiteral("/icons");
    pastasIcones << QDir::homePath() + QStringLiteral("/.icons");
    for (const QString& d : pastasIcones) if (!caminhos.contains(d) && QDir(d).exists()) caminhos << d;
    QIcon::setThemeSearchPaths(caminhos);
    QIcon::setFallbackSearchPaths(QIcon::fallbackSearchPaths() << QStringLiteral("/usr/share/pixmaps"));  // ícones soltos
    if (!tema.isEmpty()) QIcon::setThemeName(tema);
    std::vector<AppDesktop> apps = LerFixos(pasta);
    std::printf("liquinix-dock: %s; ajustes em %s; %zu apps fixos; ícone %.0f, ampliação %s (%.0f); ícones: %s\n", safeMode ? "modo seguro" : "normal",
                qPrintable(pasta), apps.size(), tile, magnify ? "ligada" : "desligada", large, qPrintable(QIcon::themeName()));
    std::fflush(stdout);  // a saída vai para um arquivo ou cano: sem isto a linha só sairia ao fechar

    DockWindow w(std::move(apps), tile, large, magnify);
    if (const char* png = std::getenv("LIQUINIX_DOCK_PNG")) {  // teste: desenha numa imagem e sai (CI e conferência)
        const char* mx = std::getenv("LIQUINIX_DOCK_PNG_MOUSE");
        bool ok = w.SalvarPng(QString::fromLocal8Bit(png), 1280, mx ? float(std::atof(mx)) : -1.f, 2.0);
        std::printf("liquinix-dock: imagem %s: %s\n", png, ok ? "ok" : "falhou");
        return ok ? 0 : 1;
    }
    QSurfaceFormat fmt = w.format(); fmt.setAlphaBufferSize(8); w.setFormat(fmt);  // fundo transparente
    w.resize(1, w.AlturaJanela());  // a largura vem do compositor (presa à esquerda e à direita)
    if (auto* ls = LayerShellQt::Window::get(&w)) {  // antes de mostrar: vira uma superfície de layer-shell
        ls->setScope(QStringLiteral("liquinix-dock"));
        ls->setLayer(LayerShellQt::Window::LayerTop);
        ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(0);  // por cima das janelas, sem reservar espaço (como o Dock do Mac)
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    }
    w.show();
    return app.exec();
}
