// InkFrame app entry (Qt6). Engine is clean-room; no Krita code linked.
#include "ui/InkMainWindow.h"
#include <QApplication>

int main(int argc, char **argv) {
    // Full-rate pointer stream for strokes (Krita main.cc concept): Qt must
    // not coalesce high-frequency move events; the stroke path does its own
    // spacing/smoothing. Pan/zoom gestures unaffected.
    QApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("InkFrame"));
    app.setOrganizationName(QStringLiteral("InkFrame"));
    ink::MainWindow w;
    w.show();
    return app.exec();
}
