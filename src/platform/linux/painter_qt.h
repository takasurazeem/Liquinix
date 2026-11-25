// drawing::Painter over QPainter (Linux, Qt 6). Draws into an open QPainter; the interface font comes from the caller
// (for now the system's). Coordinates in points: the QPainter is already at the device's scale.
#pragma once
#include "../../core/drawing.h"
#include <QFont>
#include <QPainter>

class PainterQt : public drawing::Painter {
public:
    explicit PainterQt(QPainter& p, const QString& family = QString()) : p_(p), family_(family) {}

    void FillRoundedRect(const drawing::Rect& r, float radius, const drawing::Color& c) override;
    float TextWidth(const std::wstring& s, float size, drawing::Weight w) override;
    void Text(const std::wstring& s, const drawing::Rect& r, float size, drawing::Weight w, drawing::Align a, const drawing::Color& c) override;

private:
    QFont Font(float size, drawing::Weight w) const;
    QPainter& p_;
    QString family_;
};
