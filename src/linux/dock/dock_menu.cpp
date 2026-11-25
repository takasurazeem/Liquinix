// The Dock's right-click menu on Linux (see dock_menu.h).
#include "dock_menu.h"
#include "platform/linux/painter_qt.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <algorithm>

namespace {
const float kPad = 5, kTextSize = 13, kRadius = 10;
}  // namespace

DockMenu::DockMenu(QWindow* parent, std::vector<Item> items, std::function<void(int)> chosen)
    : items_(std::move(items)), chosen_(std::move(chosen)) {
    setFlags(Qt::Popup | Qt::FramelessWindowHint);
    setTransientParent(parent);  // a popup of the Dock's surface (xdg_popup), placed relative to it
    QSurfaceFormat fmt = format(); fmt.setAlphaBufferSize(8); setFormat(fmt);
}

float DockMenu::ItemTop(int i) const {
    float y = kPad;
    for (int k = 0; k < i; k++) y += ItemHeight(k);
    return y;
}

int DockMenu::ItemAt(float y) const {
    for (int i = 0; i < int(items_.size()); i++)
        if (!items_[i].separator && y >= ItemTop(i) && y < ItemTop(i) + ItemHeight(i)) return i;
    return -1;
}

void DockMenu::Open(const QPoint& anchor) {
    // as wide as the longest label (measured with the same painter that draws it), at least 160 points
    QImage probe(1, 1, QImage::Format_ARGB32_Premultiplied);
    QPainter pp(&probe);
    PainterQt measure(pp);
    float w = 160;
    for (auto& it : items_) if (!it.separator) w = std::max(w, measure.TextWidth(it.label.toStdWString(), kTextSize, drawing::Weight::Normal) + 44);
    pp.end();
    width_ = w;
    int h = int(ItemTop(int(items_.size())) + kPad);
    resize(int(w), h);
    setPosition(anchor - QPoint(int(w) / 2, h));
    show();
    requestActivate();
}

bool DockMenu::Choose(const QString& label) {
    for (auto& it : items_)
        if (!it.separator && it.label == label) { int id = it.id; close(); chosen_(id); return true; }
    return false;
}

void DockMenu::paintEvent(QPaintEvent*) {
    QPainter p(this);
    Draw(p, true);
}

bool DockMenu::SavePng(const QString& file) {
    QImage img(width() * 2, height() * 2, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(2);
    img.fill(QColor(70, 90, 120));
    QPainter p(&img);
    Draw(p, false);
    p.end();
    return img.save(file);
}

void DockMenu::Draw(QPainter& p, bool clear) {
    p.setRenderHint(QPainter::Antialiasing);
    if (clear) {
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    PainterQt pq(p);
    bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    drawing::Color panel = dark ? drawing::Color{ 0.17f, 0.17f, 0.18f, 0.97f } : drawing::Color{ 0.96f, 0.96f, 0.97f, 0.97f };
    drawing::Color text = dark ? drawing::Color{ 1, 1, 1, 0.92f } : drawing::Color{ 0.1f, 0.1f, 0.1f, 1 };
    pq.FillRoundedRect(drawing::Rect{ 0, 0, float(width()), float(height()) }, kRadius, panel);
    for (int i = 0; i < int(items_.size()); i++) {
        float top = ItemTop(i), h = ItemHeight(i);
        if (items_[i].separator) {
            pq.FillRoundedRect(drawing::Rect{ 12, top + h / 2 - 0.5f, float(width()) - 12, top + h / 2 + 0.5f }, 0,
                               dark ? drawing::Color{ 1, 1, 1, 0.12f } : drawing::Color{ 0, 0, 0, 0.10f });
            continue;
        }
        bool hot = i == hover_;
        if (hot) pq.FillRoundedRect(drawing::Rect{ kPad, top, float(width()) - kPad, top + h }, 6, drawing::Color{ 0.04f, 0.52f, 1.0f, 1 });  // the accent blue
        pq.Text(items_[i].label.toStdWString(), drawing::Rect{ 16, top, float(width()) - 16, top + h }, kTextSize, drawing::Weight::Normal,
                drawing::Align::Leading, hot ? drawing::Color{ 1, 1, 1, 1 } : text);
    }
}

void DockMenu::mouseMoveEvent(QMouseEvent* e) {
    int h = ItemAt(float(e->position().y()));
    if (h != hover_) { hover_ = h; update(); }
}

void DockMenu::mouseReleaseEvent(QMouseEvent* e) {
    int i = ItemAt(float(e->position().y()));
    if (i < 0) return;
    int id = items_[i].id;
    close();
    chosen_(id);
}

void DockMenu::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) close();
}

bool DockMenu::event(QEvent* e) {
    if (e->type() == QEvent::Leave && hover_ >= 0) { hover_ = -1; update(); }
    return QRasterWindow::event(e);
}
