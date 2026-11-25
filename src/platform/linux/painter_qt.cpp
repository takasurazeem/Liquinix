// drawing::Painter over QPainter (see painter_qt.h).
#include "painter_qt.h"
#include <QFontMetricsF>
#include <QPainterPath>

namespace {
QRectF R(const drawing::Rect& r) { return QRectF(QPointF(r.left, r.top), QPointF(r.right, r.bottom)); }
QColor C(const drawing::Color& c) { return QColor::fromRgbF(c.r, c.g, c.b, c.a); }
}  // namespace

QFont PainterQt::Font(float size, drawing::Weight w) const {
    QFont f = family_.isEmpty() ? QFont() : QFont(family_);
    f.setPointSizeF(qMax(0.75f, size * 0.75f));  // size in 1/96" units (DIP): 0.75 pt each (Qt uses 96 logical dpi)
    f.setWeight(QFont::Weight(int(w)));
    return f;
}

void PainterQt::FillRoundedRect(const drawing::Rect& r, float radius, const drawing::Color& c) {
    QPainterPath path;
    path.addRoundedRect(R(r), radius, radius);
    p_.fillPath(path, C(c));
}

float PainterQt::TextWidth(const std::wstring& s, float size, drawing::Weight w) {
    return float(QFontMetricsF(Font(size, w)).horizontalAdvance(QString::fromStdWString(s)));
}

void PainterQt::Text(const std::wstring& s, const drawing::Rect& r, float size, drawing::Weight w, drawing::Align a, const drawing::Color& c) {
    int h = a == drawing::Align::Center ? Qt::AlignHCenter : a == drawing::Align::Trailing ? Qt::AlignRight : Qt::AlignLeft;
    p_.save();
    p_.setFont(Font(size, w));
    p_.setPen(C(c));
    p_.drawText(R(r), h | Qt::AlignVCenter | Qt::TextSingleLine, QString::fromStdWString(s));
    p_.restore();
}
