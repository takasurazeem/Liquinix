// Downloads as a stack (see dock_stack.h).
#include "dock_stack.h"
#include "platform/linux/painter_qt.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QFontMetricsF>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>

namespace {
const float kPad = 14, kHead = 36, kCellW = 92, kCellH = 100, kIcon = 56, kButtonH = 34, kRadius = 14;
}  // namespace

DockStack::DockStack(QWindow* parent, QString title, std::vector<File> files, QString folderButton, QString emptyText,
                     std::function<void(const QString&)> chosen)
    : title_(std::move(title)), button_(std::move(folderButton)), empty_(std::move(emptyText)), files_(std::move(files)), chosen_(std::move(chosen)) {
    setFlags(Qt::Popup | Qt::FramelessWindowHint);
    setTransientParent(parent);  // a popup of the Dock's surface (xdg_popup), placed relative to it
    QSurfaceFormat fmt = format(); fmt.setAlphaBufferSize(8); setFormat(fmt);
    cols_ = std::clamp(int(files_.size()), 3, 4);
}

QRectF DockStack::Cell(int i) const {
    return QRectF(kPad + (i % cols_) * kCellW, kHead + (i / cols_) * kCellH, kCellW, kCellH);
}

QRectF DockStack::Button() const {
    int rows = files_.empty() ? 1 : int(files_.size() + cols_ - 1) / cols_;
    return QRectF(kPad, kHead + rows * kCellH + 6, width() - 2 * kPad, kButtonH);
}

int DockStack::HitAt(const QPointF& p) const {
    for (int i = 0; i < int(files_.size()); i++) if (Cell(i).contains(p)) return i;
    return Button().contains(p) ? -2 : -1;
}

void DockStack::Open(const QPoint& anchor) {
    int rows = files_.empty() ? 1 : int(files_.size() + cols_ - 1) / cols_;
    int w = int(2 * kPad + cols_ * kCellW), h = int(kHead + rows * kCellH + 6 + kButtonH + kPad);
    resize(w, h);
    setPosition(anchor - QPoint(w / 2, h));
    show();
    requestActivate();
}

bool DockStack::Choose(const QString& name) {
    if (name == button_) { close(); chosen_(QString()); return true; }
    for (auto& f : files_) if (f.name == name) { QString path = f.path; close(); chosen_(path); return true; }
    return false;
}

bool DockStack::SavePng(const QString& file) {
    QImage img(width() * 2, height() * 2, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(2);
    img.fill(QColor(70, 90, 120));
    QPainter p(&img);
    Draw(p, false);
    p.end();
    return img.save(file);
}

void DockStack::paintEvent(QPaintEvent*) {
    QPainter p(this);
    Draw(p, true);
}

void DockStack::Draw(QPainter& p, bool clear) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (clear) {
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, width(), height()), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    PainterQt pq(p);
    bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    drawing::Color panel = dark ? drawing::Color{ 0.17f, 0.17f, 0.18f, 0.97f } : drawing::Color{ 0.96f, 0.96f, 0.97f, 0.97f };
    drawing::Color text = dark ? drawing::Color{ 1, 1, 1, 0.92f } : drawing::Color{ 0.1f, 0.1f, 0.1f, 1 };
    drawing::Color dim = dark ? drawing::Color{ 1, 1, 1, 0.55f } : drawing::Color{ 0, 0, 0, 0.5f };
    pq.FillRoundedRect(drawing::Rect{ 0, 0, float(width()), float(height()) }, kRadius, panel);
    pq.Text(title_.toStdWString(), drawing::Rect{ kPad, 6, float(width()) - kPad, kHead - 4 }, 14, drawing::Weight::Semibold, drawing::Align::Center, text);
    if (files_.empty())
        pq.Text(empty_.toStdWString(), drawing::Rect{ kPad, kHead, float(width()) - kPad, kHead + kCellH }, 13, drawing::Weight::Normal, drawing::Align::Center, dim);
    qreal dpr = p.device()->devicePixelRatio();
    for (int i = 0; i < int(files_.size()); i++) {
        QRectF c = Cell(i);
        if (i == hover_) pq.FillRoundedRect(drawing::Rect{ float(c.left()) + 3, float(c.top()) + 2, float(c.right()) - 3, float(c.bottom()) - 2 }, 8,
                                            dark ? drawing::Color{ 1, 1, 1, 0.12f } : drawing::Color{ 0, 0, 0, 0.08f });
        QPixmap px = files_[i].icon.pixmap(QSize(int(kIcon), int(kIcon)), dpr);
        p.drawPixmap(QRectF(c.center().x() - kIcon / 2, c.top() + 8, kIcon, kIcon), px, QRectF(0, 0, px.width(), px.height()));
        // the name under the icon, shortened with an ellipsis to the cell's width
        QFont f; f.setPointSizeF(11 * 0.75);  // the size PainterQt draws 11-point text at
        QString name = QFontMetricsF(f).elidedText(files_[i].name, Qt::ElideMiddle, kCellW - 8);
        pq.Text(name.toStdWString(), drawing::Rect{ float(c.left()) + 4, float(c.top()) + 8 + kIcon + 4, float(c.right()) - 4, float(c.top()) + 8 + kIcon + 22 },
                11, drawing::Weight::Normal, drawing::Align::Center, text);
    }
    QRectF b = Button();
    pq.FillRoundedRect(drawing::Rect{ float(b.left()), float(b.top()), float(b.right()), float(b.bottom()) }, 8,
                       hover_ == -2 ? drawing::Color{ 0.04f, 0.52f, 1.0f, 1 } : (dark ? drawing::Color{ 1, 1, 1, 0.10f } : drawing::Color{ 0, 0, 0, 0.06f }));
    pq.Text((button_ + QStringLiteral("  ›")).toStdWString(), drawing::Rect{ float(b.left()), float(b.top()), float(b.right()), float(b.bottom()) },
            13, drawing::Weight::Normal, drawing::Align::Center, hover_ == -2 ? drawing::Color{ 1, 1, 1, 1 } : text);
}

void DockStack::mouseMoveEvent(QMouseEvent* e) {
    int h = HitAt(e->position());
    if (h != hover_) { hover_ = h; update(); }
}

void DockStack::mouseReleaseEvent(QMouseEvent* e) {
    int h = HitAt(e->position());
    if (h == -1) return;
    QString path = h == -2 ? QString() : files_[h].path;
    close();
    chosen_(path);
}

void DockStack::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) close();
}

bool DockStack::event(QEvent* e) {
    if (e->type() == QEvent::Leave && hover_ != -1) { hover_ = -1; update(); }
    return QRasterWindow::event(e);
}
