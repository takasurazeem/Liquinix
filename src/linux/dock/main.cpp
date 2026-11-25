// liquinix-dock: Liquinix's Dock on Linux (Qt 6, layer-shell; see docs/linux-roadmap.md and issue #20).
// A layer-shell surface anchored to the bottom edge, as wide as the screen, with the Dock drawn in the middle.
// The layout (dock_layout), the curves and the drawing primitives are the Windows Dock's own code (src/core); only what
// is Linux-specific lives here: the apps (.desktop files), the theme icons, launching, the window and pointer input.
// Frames only while something moves (update() -> requestUpdate, paced by the compositor); idle, nothing runs.
#include "apps.h"
#include "background_effect.h"
#include "window_list.h"
#include "core/dock_layout.h"
#include "platform/linux/painter_qt.h"
#include "platform/platform.h"

#include <LayerShellQt/Window>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLineF>
#include <QGuiApplication>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRasterWindow>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QStyleHints>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace {

// ajustes.txt (the same format as on Windows: key=value per line)
std::map<QString, QString> ReadSettings(const QString& dir) {
    std::map<QString, QString> m;
    QFile f(dir + QStringLiteral("/ajustes.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString l = in.readLine();
        int eq = l.indexOf('=');
        if (eq > 0) m[l.left(eq).trimmed()] = l.mid(eq + 1).trimmed();
    }
    return m;
}

// one slot in the Dock: a pinned app, the separator, the Downloads folder or the Trash (the Windows Dock's order)
struct Entry {
    enum Kind { App, Separator, Downloads, Trash } kind;
    int app = -1;     // App: index in apps_
    QIcon icon;
    QString label;    // shown on hover
};

// the XDG trash (Trash specification): $XDG_DATA_HOME/Trash/files
QString TrashFilesDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash/files"); }
bool TrashHasFiles() { return !QDir(TrashFilesDir()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System); }

class DockWindow : public QRasterWindow {
public:
    DockWindow(std::vector<DesktopApp> apps, float tile, float large, bool magnify, QString settingsDir)
        : apps_(std::move(apps)), tile_(tile), large_(large), magnify_(magnify), settingsDir_(std::move(settingsDir)) {
        for (const DesktopApp& a : apps_)
            appIcons_.push_back(a.icon.startsWith('/') ? QIcon(a.icon) : QIcon::fromTheme(a.icon, QIcon::fromTheme(QStringLiteral("application-x-executable"))));
        BuildEntries();
        UpdateTrash();
        // the Trash icon follows its contents: a watcher on the trash folder (and its parents, so a first trashed file
        // that creates the folder is seen too), never polling
        QString files = TrashFilesDir();
        for (const QString& d : { files, QFileInfo(files).path(), QFileInfo(QFileInfo(files).path()).path() })
            if (QDir(d).exists()) trashWatcher_.addPath(d);
        QObject::connect(&trashWatcher_, &QFileSystemWatcher::directoryChanged, [this, files] {
            if (QDir(files).exists() && !trashWatcher_.directories().contains(files)) trashWatcher_.addPath(files);
            UpdateTrash();
        });
        setFlag(Qt::FramelessWindowHint);
        running_.assign(apps_.size(), false);
    }
    // Open windows (Wayland only, and only if KWin offers the protocol): the "running" dots, and clicks that bring an
    // open app to the front instead of starting it again
    void ConnectWindowList(WindowList* list) {
        windowList_ = list;
        QObject::connect(list, &WindowList::Changed, [this] { UpdateRunning(); });
        auto report = [this] {
            std::printf("liquinix-dock: Plasma window list %s\n", windowList_->Available() ? "connected" : "not available yet");
            std::fflush(stdout);
            UpdateRunning();
        };
        QObject::connect(list, &WindowList::activeChanged, report);
        report();  // it may have connected already, before this signal was hooked up
    }
    // The compositor's blur behind the shelf (Wayland only, not in safe mode)
    void ConnectBackgroundEffect(BackgroundEffect* effect) {
        effect_ = effect;
        auto report = [this] {
            std::printf("liquinix-dock: blur %s\n", effect_->BlurSupported() ? "available" : "not available (opaque shelf)");
            std::fflush(stdout);
            blurRegion_ = QRegion();  // send the region again
            update();
        };
        QObject::connect(effect, &BackgroundEffect::CapabilitiesChanged, report);
        report();
    }
    // Tahoe measurements (the same as the Windows Dock, src/shell/dock.cpp)
    float PadX() const { return tile_ * 0.125f; }
    float Slot() const { return tile_ * 1.0625f; }
    float PadTop() const { return tile_ * 0.156f; }
    float PadBottom() const { return tile_ * 0.148f; }
    float Margin() const { return tile_ * 0.10f; }
    float Radius() const { return tile_ * 0.40f; }
    int WindowHeight() const { return int(std::ceil(large_ + PadTop() + PadBottom() + Margin() + 52)); }

protected:
    void paintEvent(QPaintEvent*) override {
        double now = platform::NowMs();
        float dt = last_ > 0 ? float(std::clamp((now - last_) / 1000.0, 0.0, 0.05)) : 0.016f;
        last_ = now;
        // magnification reaches its target in ~0.15 s (exponential in time, the same at 60 or 144 Hz; the Windows math)
        float target = mouseX_ >= 0 ? 1.f : 0.f;
        magAmt_ += (target - magAmt_) * (1 - std::exp(-dt / 0.05f));
        if (std::fabs(target - magAmt_) < 0.002f) magAmt_ = target;
        Layout(float(width()), float(height()));
        UpdateBlur();  // before drawing: KWin applies the new region with this frame's commit
        QPainter p(this);
        Draw(p, float(width()), float(height()));
        p.end();
        UpdateMask();
        bool animating = magAmt_ != target || dragging_;
        if (animating) update(); else last_ = 0;  // another frame only while something still moves
    }

public:
    QString PinnedIds() const { QStringList ids; for (auto& a : apps_) ids << a.id; return ids.join(QStringLiteral(", ")); }
    // LIQUINIX_DOCK_PNG: the same drawing into an image (no window), with the pointer at `mouse` and full magnification
    bool SavePng(const QString& file, int W, float mouse, qreal scale) {
        mouseX_ = lastMouseX_ = mouse; magAmt_ = mouse >= 0 ? 1.f : 0.f;
        int H = WindowHeight();
        Layout(float(W), float(H));
        hover_ = -1;
        for (size_t i = 0; mouse >= 0 && i < items_.size(); i++) if (std::fabs(mouse - items_[i].cx) <= Slot() * items_[i].size / tile_ / 2) hover_ = int(i);
        QImage img(int(W * scale), int(H * scale), QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(scale);
        img.fill(QColor(70, 90, 120));  // a plain "wallpaper" to see the transparency against
        QPainter p(&img);
        Draw(p, float(W), float(H));
        p.end();
        return img.save(file);
    }

protected:
    void Draw(QPainter& p, float W, float H) {
        (void)W; (void)H;
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (p.device() == this) {  // in the window: clear to transparent (the test image keeps its own background)
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        PainterQt pq(p);
        // the background: with KWin's blur behind it, the Windows Dock's Tahoe glass tint (measured on macOS 26: a 0.56
        // grey at 39% in light mode, 0.28 in dark mode); without blur, an opaque light shelf
        bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        drawing::Color shelf = !blurOn_ ? drawing::Color{ 0.96f, 0.96f, 0.97f, 0.78f }
                              : dark ? drawing::Color{ 0.28f, 0.28f, 0.29f, 0.39f } : drawing::Color{ 0.56f, 0.56f, 0.57f, 0.39f };
        pq.FillRoundedRect(bg_, Radius(), shelf);
        QPainterPath border; border.addRoundedRect(QRectF(bg_.left + 0.5, bg_.top + 0.5, bg_.right - bg_.left - 1, bg_.bottom - bg_.top - 1), Radius(), Radius());
        p.setPen(QPen(QColor(255, 255, 255, 170), 1)); p.drawPath(border);
        float baseline = bg_.bottom - PadBottom();
        qreal dpr = p.device()->devicePixelRatio();
        for (size_t i = 0; i < items_.size(); i++) {
            float s = items_[i].size, cx = items_[i].cx;
            if (dragging_ && int(i) == pressIndex_) continue;  // in the user's hand: drawn at the pointer below
            if (entries_[i].kind == Entry::Separator) {  // a thin line across the shelf (the Windows Dock's)
                float h = tile_ * 0.93f, cy = (bg_.top + bg_.bottom) / 2;
                p.setPen(QPen(dark ? QColor::fromRgbF(1, 1, 1, 0.22f) : QColor::fromRgbF(0, 0, 0, 0.18f), 1));
                p.drawLine(QPointF(cx, cy - h / 2), QPointF(cx, cy + h / 2));
                continue;
            }
            QRectF r(cx - s / 2, baseline - s, s, s);
            QPixmap px = entries_[i].icon.pixmap(QSize(int(std::ceil(s)), int(std::ceil(s))), dpr);  // QIcon caches the sizes it made
            p.drawPixmap(r, px, QRectF(0, 0, px.width(), px.height()));
            if (entries_[i].kind == Entry::App && running_[entries_[i].app]) {  // running: Tahoe's small dot, a little above the edge (the same as on Windows)
                float dotR = std::max(1.25f, tile_ * 0.028f), dotY = bg_.bottom - tile_ * 0.088f;
                pq.FillRoundedRect(drawing::Rect{ cx - dotR, dotY - dotR, cx + dotR, dotY + dotR }, dotR, drawing::Color{ 0.05f, 0.07f, 0.20f, 0.85f });
            }
        }
        if (dragging_ && pressIndex_ >= 0) {  // the dragged icon at the pointer; faded once it's far enough out to be removed
            float s = tile_;
            QPixmap px = entries_[pressIndex_].icon.pixmap(QSize(int(std::ceil(s)), int(std::ceil(s))), dpr);
            p.setOpacity(outside_ ? 0.55 : 1.0);
            p.drawPixmap(QRectF(mouseX_ - s / 2, dragY_ - s / 2, s, s), px, QRectF(0, 0, px.width(), px.height()));
            p.setOpacity(1.0);
        }
        // the name of the app under the pointer, in a label above its icon (as on the Mac)
        if (hover_ >= 0 && magAmt_ > 0.5f) {
            float s = items_[hover_].size, cx = items_[hover_].cx;
            std::wstring name = entries_[hover_].label.toStdWString();
            float tw = pq.TextWidth(name, 13, drawing::Weight::Normal);
            if (tw >= 0) {
                float w = tw + 24, h = 26, top = baseline - s - 12 - h;
                drawing::Rect r{ cx - w / 2, top, cx + w / 2, top + h };
                pq.FillRoundedRect(r, 8, drawing::Color{ 0.95f, 0.95f, 0.96f, 0.92f });
                pq.Text(name, r, 13, drawing::Weight::Normal, drawing::Align::Center, drawing::Color{ 0.1f, 0.1f, 0.1f, 1 });
            }
        }
    }

    bool event(QEvent* e) override {
        if (e->type() == QEvent::Leave) { mouseX_ = -1; hover_ = -1; update(); }
        return QRasterWindow::event(e);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        pressIndex_ = hover_ >= 0 && entries_[hover_].kind == Entry::App ? hover_ : -1;  // only pinned apps can be dragged
        pressPos_ = e->position();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        float x = float(e->position().x()), y = float(e->position().y());
        if (pressIndex_ >= 0 && (e->buttons() & Qt::LeftButton)) {
            if (!dragging_ && QLineF(pressPos_, e->position()).length() > 6) { dragging_ = true; hover_ = -1; }
            if (dragging_) {  // the icon follows the pointer; well above the Dock it would be removed
                mouseX_ = lastMouseX_ = x; dragY_ = y;
                outside_ = y < bg_.top - large_;
                update();
                return;
            }
        }
        bool inside = x >= bg_.left - large_ && x <= bg_.right + large_ && y >= bg_.top - (large_ - tile_) - 20;
        mouseX_ = inside ? x : -1;
        if (inside) lastMouseX_ = x;
        int h = -1;
        for (size_t i = 0; inside && i < items_.size(); i++)
            if (!items_[i].separator && std::fabs(x - items_[i].cx) <= Slot() * items_[i].size / tile_ / 2) h = int(i);
        hover_ = h;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && dragging_) { FinishDrag(); return; }
        pressIndex_ = -1;
        if (e->button() != Qt::LeftButton || hover_ < 0) return;
        const Entry& en = entries_[hover_];
        if (en.kind == Entry::Downloads || en.kind == Entry::Trash) {  // open the folder in the file manager
            QUrl url = en.kind == Entry::Trash ? QUrl(QStringLiteral("trash:/")) : QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
            bool ok = QDesktopServices::openUrl(url);
            std::printf("liquinix-dock: open %s: %s\n", qPrintable(url.toString()), ok ? "ok" : "failed"); std::fflush(stdout);
            return;
        }
        if (en.kind != Entry::App) return;
        const DesktopApp& a = apps_[en.app];
        // already running: bring its window to the front (as on the Mac) instead of starting it again
        if (running_[en.app] && windowList_ && windowList_->Activate([&](const QString& id) { return WindowBelongsTo(a, id); })) {
            std::printf("liquinix-dock: activate %s\n", qPrintable(a.id)); std::fflush(stdout);
            return;
        }
        bool ok = LaunchApp(a);
        std::printf("liquinix-dock: launch %s (%s): %s\n", qPrintable(a.id), qPrintable(a.exec), ok ? "ok" : "failed");
        std::fflush(stdout);
    }
    void resizeEvent(QResizeEvent*) override { update(); }

private:
    void UpdateRunning() {
        QStringList ids = windowList_ && windowList_->Available() ? windowList_->AppIds() : QStringList();
        std::vector<bool> now(apps_.size(), false);
        for (size_t i = 0; i < apps_.size(); i++)
            for (const QString& id : ids) if (WindowBelongsTo(apps_[i], id)) { now[i] = true; break; }
        if (now == running_) return;
        running_ = now;
        QStringList names; for (size_t i = 0; i < apps_.size(); i++) if (running_[i]) names << apps_[i].id;
        std::printf("liquinix-dock: running: %s\n", names.isEmpty() ? "(none)" : qPrintable(names.join(QStringLiteral(", "))));
        std::fflush(stdout);
        update();
    }
    void UpdateTrash() {
        bool full = TrashHasFiles();
        for (auto& e : entries_)
            if (e.kind == Entry::Trash && (e.icon.isNull() || full != trashFull_))
                e.icon = QIcon::fromTheme(full ? QStringLiteral("user-trash-full") : QStringLiteral("user-trash"));
        if (full != trashFull_) { std::printf("liquinix-dock: trash %s\n", full ? "has files" : "is empty"); std::fflush(stdout); }
        trashFull_ = full;
        update();
    }
    void Layout(float W, float H) {
        std::vector<float> previous;
        if (items_.size() == entries_.size()) for (auto& it : items_) previous.push_back(it.cx);
        items_.assign(entries_.size(), {});
        for (size_t i = 0; i < entries_.size(); i++) {
            items_[i].separator = entries_[i].kind == Entry::Separator;
            items_[i].app = entries_[i].kind == Entry::App;
            items_[i].movablePinned = items_[i].app;  // every app here is pinned (there's no Finder slot on Linux)
            items_[i].appIndex = entries_[i].app;
            if (!previous.empty()) items_[i].cx = previous[i];
        }
        auto res = dock_layout::Compute(items_, LayoutParams(W));
        float bottom = H - Margin();
        bg_ = drawing::Rect{ (W - res.total) / 2, bottom - (tile_ + PadTop() + PadBottom()), (W + res.total) / 2, bottom };
    }
    // the shared layout's inputs from the Dock's current state
    dock_layout::Params LayoutParams(float W) const {
        dock_layout::Params prm;
        prm.axis = W; prm.icon = tile_; prm.magnified = large_; prm.step = Slot(); prm.margin = PadX();
        prm.separatorWidth = tile_ * 0.484f; prm.magnify = magnify_; prm.tahoe = true;
        prm.mouse = mouseX_; prm.lastMouse = lastMouseX_; prm.magnification = magAmt_;
        prm.dragging = dragging_; prm.dragged = dragging_ ? pressIndex_ : -1; prm.outsideDock = outside_;
        return prm;
    }
    // the slots: the pinned apps, then the separator, Downloads and the Trash (the Windows Dock's order)
    void BuildEntries() {
        QIcon trash = entries_.empty() ? QIcon() : entries_.back().icon;
        entries_.clear();
        for (size_t i = 0; i < apps_.size(); i++) entries_.push_back({ Entry::App, int(i), appIcons_[i], apps_[i].name });
        entries_.push_back({ Entry::Separator, -1, QIcon(), QString() });
        entries_.push_back({ Entry::Downloads, -1, QIcon::fromTheme(QStringLiteral("folder-downloads"), QIcon::fromTheme(QStringLiteral("folder"))), QStringLiteral("Downloads") });
        entries_.push_back({ Entry::Trash, -1, trash, QStringLiteral("Trash") });
        items_.clear();  // a different set of slots: no centres to carry over
    }
    // a drag ended: move the app to where the gap opened, or unpin it if it was dropped well above the Dock
    void FinishDrag() {
        int from = entries_[pressIndex_].app;
        // where it lands, from the pointer's position now (on a copy: the release can come before the next frame)
        std::vector<dock_layout::Item> copy = items_;
        int dropTarget = dock_layout::Compute(copy, LayoutParams(float(width()))).dropTarget;
        bool changed = false;
        if (outside_) {
            std::printf("liquinix-dock: unpin %s\n", qPrintable(apps_[from].id));
            apps_.erase(apps_.begin() + from); appIcons_.erase(appIcons_.begin() + from);
            changed = true;
        } else if (dropTarget >= 0 && dropTarget != from) {
            int to = std::clamp(dropTarget, 0, int(apps_.size()) - 1);  // the same move as the Windows Dock (apps::Move)
            std::printf("liquinix-dock: move %s from %d to %d\n", qPrintable(apps_[from].id), from, to);
            DesktopApp a = apps_[from]; QIcon icon = appIcons_[from];
            apps_.erase(apps_.begin() + from); appIcons_.erase(appIcons_.begin() + from);
            apps_.insert(apps_.begin() + to, a); appIcons_.insert(appIcons_.begin() + to, icon);
            changed = true;
        }
        dragging_ = false; outside_ = false; pressIndex_ = -1;
        if (changed) {
            BuildEntries();
            running_.assign(apps_.size(), false);
            UpdateRunning();
            if (!WritePinned(settingsDir_, apps_)) std::printf("liquinix-dock: couldn't save %s/dock.txt\n", qPrintable(settingsDir_));
        }
        std::fflush(stdout);
        update();
    }
    // the compositor's blur behind the shelf only (its rounded shape), sent again only when the shape changes
    void UpdateBlur() {
        if (!effect_ || !effect_->BlurSupported()) { blurOn_ = false; return; }
        QPainterPath shape;
        shape.addRoundedRect(QRectF(bg_.left, bg_.top, bg_.right - bg_.left, bg_.bottom - bg_.top), Radius(), Radius());
        QRegion region(shape.toFillPolygon().toPolygon());
        if (region == blurRegion_) return;
        blurRegion_ = region;
        blurOn_ = effect_->SetBlurRegion(this, region);
    }
    // only the Dock takes the pointer; the rest of the strip lets clicks through to the windows below. With the pointer
    // on it, the area grows upwards (the magnified icons) and sideways.
    void UpdateMask() {
        QRect r = mouseX_ >= 0 ? QRectF(bg_.left - large_, 0, bg_.right - bg_.left + 2 * large_, height()).toAlignedRect()
                               : QRectF(bg_.left, bg_.top, bg_.right - bg_.left, bg_.bottom - bg_.top).toAlignedRect();
        if (r != mask_) { mask_ = r; setMask(QRegion(r)); }
    }

    std::vector<DesktopApp> apps_;
    std::vector<QIcon> appIcons_;         // per pinned app, in the same order as apps_
    int pressIndex_ = -1;                 // entry pressed with the left button (a pinned app), -1 = none
    QPointF pressPos_;
    bool dragging_ = false, outside_ = false;  // dragging that app; dropped now it would be unpinned
    float dragY_ = 0;
    std::vector<Entry> entries_;          // what the Dock shows, in order (items_ is laid out from it)
    QFileSystemWatcher trashWatcher_;
    bool trashFull_ = false;
    std::vector<dock_layout::Item> items_;
    std::vector<bool> running_;           // per pinned app: has an open window
    WindowList* windowList_ = nullptr;
    float tile_, large_;
    bool magnify_;
    QString settingsDir_;                 // where dock.txt is saved
    BackgroundEffect* effect_ = nullptr;
    bool blurOn_ = false;      // blur requested: draw the glass tint instead of the opaque shelf
    QRegion blurRegion_;
    float mouseX_ = -1, lastMouseX_ = -1, magAmt_ = 0;
    int hover_ = -1;
    double last_ = 0;
    drawing::Rect bg_;
    QRect mask_;
};

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("liquinix-dock"));
    const char* safe = std::getenv("LIQUINIX_SAFE_MODE");
    bool safeMode = safe && std::strcmp(safe, "1") == 0;  // safe mode: nothing optional (no magnification)
    QString dir = QString::fromStdWString(platform::SettingsDir());
    auto settings = ReadSettings(dir);
    auto num = [&](const char* k, float def) { auto it = settings.find(QString::fromLatin1(k)); bool ok = false; float v = it == settings.end() ? def : it->second.toFloat(&ok); return ok ? v : def; };
    float tile = std::clamp(num("tamanho", 48), 16.f, 128.f);
    float large = std::clamp(num("ampliacao_tamanho", 128), tile, 256.f);
    bool magnify = !safeMode && settings["ampliacao"] == QLatin1String("1");
    // icon theme: the one in the settings (tema_icones=MacTahoe, for example), else KDE's (kdeglobals), else whatever Qt
    // found; with none of those (no platform theme, as in image mode), Breeze
    QString theme = settings["tema_icones"];
    if (theme.isEmpty()) {
        QString kg = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
        if (!kg.isEmpty()) theme = QSettings(kg, QSettings::IniFormat).value(QStringLiteral("Icons/Theme")).toString();
    }
    if (theme.isEmpty() && QIcon::themeName().isEmpty()) theme = QStringLiteral("breeze");
    // where to look for themes (Icon Theme Specification): <XDG_DATA_DIRS>/icons and ~/.icons. The platform theme (KDE)
    // usually adds these; without it Qt only looks at its built-in icons
    QStringList searchPaths = QIcon::themeSearchPaths();
    QStringList iconDirs;  // (a named list: a range-for over the temporary of "list << item" would use freed memory)
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) iconDirs << d + QStringLiteral("/icons");
    iconDirs << QDir::homePath() + QStringLiteral("/.icons");
    for (const QString& d : iconDirs) if (!searchPaths.contains(d) && QDir(d).exists()) searchPaths << d;
    QIcon::setThemeSearchPaths(searchPaths);
    QIcon::setFallbackSearchPaths(QIcon::fallbackSearchPaths() << QStringLiteral("/usr/share/pixmaps"));  // loose icons
    if (!theme.isEmpty()) QIcon::setThemeName(theme);
    std::vector<DesktopApp> apps = ReadPinned(dir);
    std::printf("liquinix-dock: %s; settings in %s; %zu pinned apps; icon %.0f, magnification %s (%.0f); icons: %s\n", safeMode ? "safe mode" : "normal",
                qPrintable(dir), apps.size(), tile, magnify ? "on" : "off", large, qPrintable(QIcon::themeName()));
    std::fflush(stdout);  // output goes to a file or pipe: without this the line would only appear on exit

    DockWindow w(std::move(apps), tile, large, magnify, dir);
    if (const char* png = std::getenv("LIQUINIX_DOCK_PNG")) {  // test: draw into an image and exit (CI and checks)
        const char* mx = std::getenv("LIQUINIX_DOCK_PNG_MOUSE");
        bool ok = w.SavePng(QString::fromLocal8Bit(png), 1280, mx ? float(std::atof(mx)) : -1.f, 2.0);
        std::printf("liquinix-dock: image %s: %s\n", png, ok ? "ok" : "failed");
        return ok ? 0 : 1;
    }
    if (const char* script = std::getenv("LIQUINIX_DOCK_SCRIPT")) {
        // test: real Qt mouse events from a script ("move x y; press; move x y; release"), on a window laid out by the
        // platform (use QT_QPA_PLATFORM=offscreen); it paints between steps, then prints the pinned order and exits
        w.resize(1280, w.WindowHeight());
        w.show();
        QPointF pos(-1, -1); Qt::MouseButtons buttons;
        auto settle = [] { for (int i = 0; i < 6; i++) QCoreApplication::processEvents(QEventLoop::AllEvents, 20); };
        settle();
        for (const QString& step : QString::fromLocal8Bit(script).split(';', Qt::SkipEmptyParts)) {
            QStringList p = step.simplified().split(' ');
            QEvent::Type type = QEvent::MouseMove;
            if (p[0] == QLatin1String("move") && p.size() == 3) pos = QPointF(p[1].toDouble(), p[2].toDouble());
            else if (p[0] == QLatin1String("press")) { type = QEvent::MouseButtonPress; buttons = Qt::LeftButton; }
            else if (p[0] == QLatin1String("release")) { type = QEvent::MouseButtonRelease; buttons = Qt::NoButton; }
            else continue;
            QMouseEvent ev(type, pos, w.mapToGlobal(pos), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&w, &ev);
            settle();
        }
        std::printf("liquinix-dock: pinned: %s\n", qPrintable(w.PinnedIds()));
        return 0;
    }
    QSurfaceFormat fmt = w.format(); fmt.setAlphaBufferSize(8); w.setFormat(fmt);  // transparent background
    w.resize(1, w.WindowHeight());  // the width comes from the compositor (anchored left and right)
    if (auto* ls = LayerShellQt::Window::get(&w)) {  // before showing: becomes a layer-shell surface
        ls->setScope(QStringLiteral("liquinix-dock"));
        ls->setLayer(LayerShellQt::Window::LayerTop);
        ls->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        ls->setExclusiveZone(0);  // above the windows, without reserving space (like the Mac's Dock)
        ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    }
    std::unique_ptr<WindowList> windowList;
    std::unique_ptr<BackgroundEffect> effect;
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        windowList = std::make_unique<WindowList>(); w.ConnectWindowList(windowList.get());
        if (!safeMode) { effect = std::make_unique<BackgroundEffect>(); w.ConnectBackgroundEffect(effect.get()); }  // safe mode: no blur
    }
    w.show();
    int result = app.exec();
    effect.reset();      // before the window and the connection to the compositor go away
    windowList.reset();
    return result;
}
