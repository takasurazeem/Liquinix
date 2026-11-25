// liquinix-dock: Liquinix's Dock on Linux (Qt 6, layer-shell; see docs/linux-roadmap.md and issue #20).
// A layer-shell surface anchored to the bottom edge, as wide as the screen, with the Dock drawn in the middle.
// The layout (dock_layout), the curves and the drawing primitives are the Windows Dock's own code (src/core); only what
// is Linux-specific lives here: the apps (.desktop files), the theme icons, launching, the window and pointer input.
// Frames only while something moves (update() -> requestUpdate, paced by the compositor); idle, nothing runs.
#include "apps.h"
#include "background_effect.h"
#include "dock_menu.h"
#include "window_list.h"
#include "core/curves.h"
#include "core/dock_layout.h"
#include "platform/linux/painter_qt.h"
#include "platform/platform.h"

#include <LayerShellQt/Window>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
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
    DockWindow(std::vector<DesktopApp> apps, float tile, float large, bool magnify, QString settingsDir, bool bounce, bool autohide)
        : apps_(std::move(apps)), tile_(tile), large_(large), magnify_(magnify), settingsDir_(std::move(settingsDir)), bounce_(bounce),
          autohide_(autohide), reveal_(autohide ? 0.f : 1.f) {
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
        launchedAt_.assign(apps_.size(), 0);
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
        // automatic hiding: slides in and out over 0.32 s (the Windows Dock's reveal, with the Mac's ease-in-out curve)
        float shown = WantShown() ? 1.f : 0.f;
        reveal_ = std::clamp(reveal_ + (shown > reveal_ ? dt : -dt) / 0.32f, 0.f, 1.f);
        if (std::fabs(shown - reveal_) < 0.001f) reveal_ = shown;
        Layout(float(width()), float(height()));
        UpdateBlur();  // before drawing: KWin applies the new region with this frame's commit
        QPainter p(this);
        Draw(p, float(width()), float(height()));
        p.end();
        UpdateMask();
        bool animating = magAmt_ != target || dragging_ || AnyBouncing(now) || reveal_ != shown;
        if (animating) update(); else last_ = 0;  // another frame only while something still moves
    }

public:
    // LIQUINIX_DOCK_SCRIPT's "png": the window's current state drawn into an image
    bool SnapshotPng(const QString& file) {
        Layout(float(width()), float(height()));
        QImage img(width() * 2, height() * 2, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(2);
        img.fill(QColor(70, 90, 120));
        QPainter p(&img);
        Draw(p, float(width()), float(height()));
        p.end();
        return img.save(file);
    }
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
        if (reveal_ <= 0.f) return;  // hidden: nothing to draw (only the edge strip takes input)
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
            float hop = entries_[i].kind == Entry::App ? BounceOffset(entries_[i].app) : 0;  // launching: the icon bounces
            QRectF r(cx - s / 2, baseline - s + hop, s, s);
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
        if (e->type() == QEvent::Leave) { mouseX_ = -1; hover_ = -1; edgeTouched_ = false; update(); }
        return QRasterWindow::event(e);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton && hover_ >= 0) { ShowMenu(hover_); return; }
        if (e->button() != Qt::LeftButton) return;
        pressIndex_ = hover_ >= 0 && entries_[hover_].kind == Entry::App ? hover_ : -1;  // only pinned apps can be dragged
        pressPos_ = e->position();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        float x = float(e->position().x()), y = float(e->position().y());
        if (autohide_ && y >= height() - 3) edgeTouched_ = true;  // the pointer reached the screen's bottom edge: come back
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
        if (!inside && y < height() - 3) edgeTouched_ = false;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && dragging_) { FinishDrag(); return; }
        pressIndex_ = -1;
        if (e->button() != Qt::LeftButton || hover_ < 0) return;
        OpenEntry(hover_);
    }
    void resizeEvent(QResizeEvent*) override { update(); }

public:
    // LIQUINIX_DOCK_SCRIPT's "menu <label>": picks an item of the open right-click menu
    bool ChooseMenu(const QString& label) { return menu_ && menu_->isVisible() && menu_->Choose(label); }
    bool MenuPng(const QString& file, int hoverY) {
        if (!menu_ || !menu_->isVisible()) return false;
        QMouseEvent mv(QEvent::MouseMove, QPointF(20, hoverY), menu_->mapToGlobal(QPointF(20, hoverY)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(menu_.get(), &mv);  // highlight the item under y, as a pointer would
        return menu_->SavePng(file);
    }

private:
    // a click on an entry: open the folder, bring a running app forward, or launch it (and bounce)
    void OpenEntry(int index) {
        const Entry& en = entries_[index];
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
        if (ok && bounce_) { launchedAt_[en.app] = platform::NowMs(); update(); }  // bounce until its window shows up
        std::printf("liquinix-dock: launch %s (%s): %s\n", qPrintable(a.id), qPrintable(a.exec), ok ? "ok" : "failed");
        std::fflush(stdout);
    }
    // the right-click menu (as on the Mac and the Windows Dock): Open or Quit, and Remove from Dock, for a pinned app;
    // Open for Downloads and the Trash
    enum MenuAction { kOpen = 1, kQuit, kRemove };
    void ShowMenu(int index) {
        const Entry& en = entries_[index];
        if (en.kind == Entry::Separator) return;
        std::vector<DockMenu::Item> items;
        bool running = en.kind == Entry::App && running_[en.app];
        items.push_back({ running ? QStringLiteral("Quit") : QStringLiteral("Open"), running ? kQuit : kOpen });
        if (en.kind == Entry::App) { items.push_back({ QString(), 0, true }); items.push_back({ QStringLiteral("Remove from Dock"), kRemove }); }
        // the choice refers to the entry by what it shows (the list can change while the menu is open)
        Entry::Kind kind = en.kind;
        QString appId = en.kind == Entry::App ? apps_[en.app].id : QString();
        menu_ = std::make_unique<DockMenu>(this, std::move(items), [this, kind, appId](int action) { MenuChosen(kind, appId, action); });
        menu_->Open(QPoint(int(items_[index].cx), int(bg_.top) - 8));
        hover_ = -1;
        update();
    }
    void MenuChosen(Entry::Kind kind, const QString& appId, int action) {
        int index = -1;
        for (int i = 0; i < int(entries_.size()); i++)
            if (entries_[i].kind == kind && (kind != Entry::App || apps_[entries_[i].app].id == appId)) { index = i; break; }
        if (index < 0) return;
        if (action == kOpen) { OpenEntry(index); return; }
        int app = entries_[index].app;
        if (action == kQuit && windowList_) {  // ask its windows to close (the app may still ask to save)
            const DesktopApp& a = apps_[app];
            int n = windowList_->CloseAll([&](const QString& id) { return WindowBelongsTo(a, id); });
            std::printf("liquinix-dock: quit %s: %d window(s) asked to close\n", qPrintable(a.id), n); std::fflush(stdout);
        } else if (action == kRemove) {
            Unpin(app);
        }
    }
    void Unpin(int app) {
        std::printf("liquinix-dock: unpin %s\n", qPrintable(apps_[app].id));
        apps_.erase(apps_.begin() + app); appIcons_.erase(appIcons_.begin() + app);
        PinnedChanged();
    }
    // the pinned list changed (moved or removed): new slots, and save dock.txt
    void PinnedChanged() {
        BuildEntries();
        running_.assign(apps_.size(), false);
        launchedAt_.assign(apps_.size(), 0);
        UpdateRunning();
        if (!WritePinned(settingsDir_, apps_)) std::printf("liquinix-dock: couldn't save %s/dock.txt\n", qPrintable(settingsDir_));
        std::fflush(stdout);
        update();
    }

private:
    void UpdateRunning() {
        QStringList ids = windowList_ && windowList_->Available() ? windowList_->AppIds() : QStringList();
        std::vector<bool> now(apps_.size(), false);
        for (size_t i = 0; i < apps_.size(); i++)
            for (const QString& id : ids) if (WindowBelongsTo(apps_[i], id)) { now[i] = true; break; }
        for (size_t i = 0; i < apps_.size(); i++) if (now[i]) launchedAt_[i] = 0;  // its window is here: stop bouncing
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
        // hidden by this much (the Windows Dock's formula: the shelf, the margin and 2 points more)
        float hide = (1 - curves::EaseInOut(reveal_)) * (tile_ + PadTop() + PadBottom() + Margin() + 2);
        float bottom = H - Margin() + hide;
        bg_ = drawing::Rect{ (W - res.total) / 2, bottom - (tile_ + PadTop() + PadBottom()), (W + res.total) / 2, bottom };
    }
    // launching: half an icon high hops of 620 ms (the Windows Dock's, curves::DockHop) for up to 8 s, until a window shows up
    bool Bouncing(int app, double now) const { return launchedAt_[app] > 0 && now - launchedAt_[app] < 8000; }
    bool AnyBouncing(double now) const { for (size_t i = 0; i < launchedAt_.size(); i++) if (Bouncing(int(i), now)) return true; return false; }
    float BounceOffset(int app) const {
        double now = platform::NowMs();
        return Bouncing(app, now) ? -curves::DockHop(now - launchedAt_[app], 620) * tile_ * 0.5f : 0.f;
    }
    // automatic hiding: shown while the pointer is on the Dock or just touched the edge, while dragging, or with the menu open
    bool WantShown() const { return !autohide_ || mouseX_ >= 0 || edgeTouched_ || dragging_ || (menu_ && menu_->isVisible()); }
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
        bool out = outside_;
        dragging_ = false; outside_ = false; pressIndex_ = -1;
        if (out) { Unpin(from); return; }
        if (dropTarget >= 0 && dropTarget != from) {
            int to = std::clamp(dropTarget, 0, int(apps_.size()) - 1);  // the same move as the Windows Dock (apps::Move)
            std::printf("liquinix-dock: move %s from %d to %d\n", qPrintable(apps_[from].id), from, to);
            DesktopApp a = apps_[from]; QIcon icon = appIcons_[from];
            apps_.erase(apps_.begin() + from); appIcons_.erase(appIcons_.begin() + from);
            apps_.insert(apps_.begin() + to, a); appIcons_.insert(appIcons_.begin() + to, icon);
            PinnedChanged();
            return;
        }
        update();
    }
    // the compositor's blur behind the shelf only (its rounded shape), sent again only when the shape changes
    void UpdateBlur() {
        if (!effect_ || !effect_->BlurSupported()) { blurOn_ = false; return; }
        QPainterPath shape;
        shape.addRoundedRect(QRectF(bg_.left, bg_.top, bg_.right - bg_.left, bg_.bottom - bg_.top), Radius(), Radius());
        QRegion region = reveal_ > 0.f ? QRegion(shape.toFillPolygon().toPolygon()) & QRegion(0, 0, width(), height()) : QRegion();
        if (region == blurRegion_) return;
        blurRegion_ = region;
        blurOn_ = effect_->SetBlurRegion(this, region);
    }
    // only the Dock takes the pointer; the rest of the strip lets clicks through to the windows below. With the pointer
    // on it, the area grows upwards (the magnified icons) and sideways.
    void UpdateMask() {
        QRect r = autohide_ && reveal_ <= 0.f ? QRect(0, height() - 2, width(), 2)  // hidden: only the edge strip
                : mouseX_ >= 0 ? QRectF(bg_.left - large_, 0, bg_.right - bg_.left + 2 * large_, height()).toAlignedRect()
                               : QRectF(bg_.left, bg_.top, bg_.right - bg_.left, bg_.bottom - bg_.top).toAlignedRect();
        if (r != mask_) { mask_ = r; setMask(QRegion(r)); }
    }

    std::vector<DesktopApp> apps_;
    std::vector<QIcon> appIcons_;         // per pinned app, in the same order as apps_
    int pressIndex_ = -1;                 // entry pressed with the left button (a pinned app), -1 = none
    QPointF pressPos_;
    bool dragging_ = false, outside_ = false;  // dragging that app; dropped now it would be unpinned
    float dragY_ = 0;
    std::unique_ptr<DockMenu> menu_;      // the right-click menu while it's open
    std::vector<Entry> entries_;          // what the Dock shows, in order (items_ is laid out from it)
    QFileSystemWatcher trashWatcher_;
    bool trashFull_ = false;
    std::vector<dock_layout::Item> items_;
    std::vector<bool> running_;           // per pinned app: has an open window
    std::vector<double> launchedAt_;      // per pinned app: when it was launched (NowMs), 0 = not bouncing
    WindowList* windowList_ = nullptr;
    float tile_, large_;
    bool magnify_;
    QString settingsDir_;                 // where dock.txt is saved
    bool bounce_;                         // bounce launching apps (animar_abertura, on by default)
    bool autohide_;                       // automatic hiding (ocultar=1)
    float reveal_;                        // 0 = hidden, 1 = shown
    bool edgeTouched_ = false;            // the pointer touched the bottom edge while hidden
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

    bool bounce = settings["animar_abertura"] != QLatin1String("0");  // the Windows setting: on unless turned off
    bool autohide = !safeMode && settings["ocultar"] == QLatin1String("1");  // automatic hiding (the Windows setting)
    DockWindow w(std::move(apps), tile, large, magnify, dir, bounce, autohide);
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
            else if (p[0] == QLatin1String("leave")) {  // the pointer leaves the window
                QEvent leave(QEvent::Leave);
                QCoreApplication::sendEvent(&w, &leave); settle();
                continue;
            }
            else if (p[0] == QLatin1String("rclick") && p.size() == 3) {  // a right click at x y (opens the menu)
                pos = QPointF(p[1].toDouble(), p[2].toDouble());
                QMouseEvent mv(QEvent::MouseMove, pos, w.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&w, &mv); settle();
                QMouseEvent pr(QEvent::MouseButtonPress, pos, w.mapToGlobal(pos), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&w, &pr); settle();
                continue;
            } else if (p[0] == QLatin1String("menupng") && p.size() == 3) {  // the open menu in an image, item at y highlighted
                w.MenuPng(p[1], p[2].toInt());
                continue;
            } else if (p[0] == QLatin1String("menu") && p.size() >= 2) {  // picks a menu item by its label
                QString label = QStringList(p.mid(1)).join(' ');
                std::printf("liquinix-dock: menu \"%s\": %s\n", qPrintable(label), w.ChooseMenu(label) ? "chosen" : "not found");
                settle();
                continue;
            }
            else if (p[0] == QLatin1String("wait") && p.size() == 2) {  // let time pass (animations step by the clock)
                QElapsedTimer t; t.start();
                while (t.elapsed() < p[1].toInt()) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                continue;
            } else if (p[0] == QLatin1String("png") && p.size() == 2) { w.SnapshotPng(p[1]); continue; }
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
