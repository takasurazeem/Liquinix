// Downloads as a stack (the Mac's and the Windows Dock's "pilha"): a popup above the icon with a grid of the newest
// files, each opening with its default app, and a button that opens the folder. Qt GUI only, drawn with
// drawing::Painter like the right-click menu; it closes on Esc, a click outside or a choice.
#pragma once
#include <QIcon>
#include <QRasterWindow>
#include <QString>
#include <functional>
#include <vector>

class DockStack : public QRasterWindow {
public:
    struct File { QString path, name; QIcon icon; };
    // `chosen` gets a file's path, or an empty string for the "open the folder" button
    DockStack(QWindow* parent, QString title, std::vector<File> files, QString folderButton, QString emptyText,
              std::function<void(const QString&)> chosen);
    void Open(const QPoint& anchor);       // bottom centre at `anchor` (the parent's coordinates)
    bool Choose(const QString& name);      // LIQUINIX_DOCK_SCRIPT: a file by name, or the folder button by its label
    bool SavePng(const QString& file);     // the stack as it looks now

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;

private:
    void Draw(QPainter& p, bool clear);
    QRectF Cell(int i) const;
    QRectF Button() const;
    int HitAt(const QPointF& p) const;     // file index, -2 = the button, -1 = nothing
    QString title_, button_, empty_;
    std::vector<File> files_;
    std::function<void(const QString&)> chosen_;
    int hover_ = -1;
    int cols_ = 4;
};
