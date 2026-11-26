// A plain window for the Linux tests (tools/linux/ci-smoke.sh): its Wayland app id is "liquinix-test-window", so a
// pinned .desktop entry of that name matches it in the Dock. It closes when the compositor asks (the Dock's Quit),
// which ends the program. Not installed or shipped.
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QTimer>
#include <cstdio>

namespace {
class TestWindow : public QRasterWindow {
protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(QRect(0, 0, width(), height()), QColor(40, 120, 200)); }
};
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("liquinix-test-window"));  // the app id
    TestWindow w;
    w.setTitle(QStringLiteral("Liquinix test window"));
    w.resize(320, 200);
    // --maximized: covers the screen (the Dock's smart hiding test); --minimize-after <ms>: minimizes itself later
    QStringList args = QCoreApplication::arguments();
    if (args.contains(QStringLiteral("--maximized"))) w.showMaximized(); else w.show();
    int i = int(args.indexOf(QStringLiteral("--minimize-after")));
    if (i >= 0 && i + 1 < args.size()) QTimer::singleShot(args[i + 1].toInt(), &w, [&w] { w.showMinimized(); std::printf("liquinix-test-window: minimized\n"); std::fflush(stdout); });
    std::printf("liquinix-test-window: shown\n"); std::fflush(stdout);
    int rc = app.exec();  // ends when the window is closed
    std::printf("liquinix-test-window: closed\n"); std::fflush(stdout);
    return rc;
}
