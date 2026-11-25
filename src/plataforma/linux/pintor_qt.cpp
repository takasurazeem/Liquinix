// desenho::Pintor sobre QPainter (ver pintor_qt.h).
#include "pintor_qt.h"
#include <QFontMetricsF>
#include <QPainterPath>

namespace {
QRectF R(const desenho::Ret& r) { return QRectF(QPointF(r.esq, r.topo), QPointF(r.dir, r.base)); }
QColor C(const desenho::Cor& c) { return QColor::fromRgbF(c.r, c.g, c.b, c.a); }
}  // namespace

QFont PintorQt::Fonte(float tamanho, desenho::Peso p) const {
    QFont f = familia_.isEmpty() ? QFont() : QFont(familia_);
    f.setPointSizeF(qMax(0.75f, tamanho * 0.75f));  // tamanho em unidades de 1/96" (DIP): 0,75 pt cada (Qt usa 96 ppp lógicos)
    f.setWeight(QFont::Weight(int(p)));
    return f;
}

void PintorQt::RetArredondado(const desenho::Ret& r, float raio, const desenho::Cor& c) {
    QPainterPath path;
    path.addRoundedRect(R(r), raio, raio);
    p_.fillPath(path, C(c));
}

float PintorQt::LarguraTexto(const std::wstring& s, float tamanho, desenho::Peso p) {
    return float(QFontMetricsF(Fonte(tamanho, p)).horizontalAdvance(QString::fromStdWString(s)));
}

void PintorQt::Texto(const std::wstring& s, const desenho::Ret& r, float tamanho, desenho::Peso p, desenho::Alinhar a, const desenho::Cor& c) {
    int h = a == desenho::Alinhar::Centro ? Qt::AlignHCenter : a == desenho::Alinhar::Direita ? Qt::AlignRight : Qt::AlignLeft;
    p_.save();
    p_.setFont(Fonte(tamanho, p));
    p_.setPen(C(c));
    p_.drawText(R(r), h | Qt::AlignVCenter | Qt::TextSingleLine, QString::fromStdWString(s));
    p_.restore();
}
