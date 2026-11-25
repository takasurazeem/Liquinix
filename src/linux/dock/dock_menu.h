// The Dock's right-click menu on Linux: a popup window (Qt GUI, no Qt Widgets) drawn with drawing::Painter, shown
// above an icon. It closes on Esc, on a click outside (the compositor ends the popup) or after a choice.
#pragma once
#include <QPainter>
#include <QRasterWindow>
#include <QString>
#include <functional>
#include <vector>

class DockMenu : public QRasterWindow {
public:
    struct Item { QString label; int id = 0; bool separator = false; };
    // `chosen` gets the item's id; the menu closes itself afterwards
    DockMenu(QWindow* parent, std::vector<Item> items, std::function<void(int)> chosen);
    // opens with its bottom centre at `anchor` (the parent's coordinates)
    void Open(const QPoint& anchor);
    // picks the item with this label as if clicked (LIQUINIX_DOCK_SCRIPT); false if there's none
    bool Choose(const QString& label);
    // the menu as it looks now, in an image (LIQUINIX_DOCK_SCRIPT's "menupng")
    bool SavePng(const QString& file);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;

private:
    void Draw(QPainter& p, bool clear);
    int ItemAt(float y) const;
    float ItemTop(int i) const;
    float ItemHeight(int i) const { return items_[i].separator ? 9.f : 26.f; }
    std::vector<Item> items_;
    std::function<void(int)> chosen_;
    int hover_ = -1;
    float width_ = 200;
};
