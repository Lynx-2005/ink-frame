// The QPainter state leak: TimelineWidget::paintEvent opened a save() it never
// restored, so Qt warned "Painter ended with 1 saved states" once per repaint.
// The timeline repaints on a 16ms tick, so the terminal filled with them.
// Painting the widget repeatedly must now leave the painter balanced.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPixmap>
#include <cstdio>

using namespace ink;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Document doc(320, 240, 48);

    GroupLayer *folder = doc.ensureFolder();
    for (int i = 0; i < 3; ++i) {
        PaintLayer *l = doc.createTrack(folder, "layer");
        PaintDevice *d = l->deviceAt(0, true);
        d->fillRect(Rect{10, 10, 100, 60}, PixelRGBA8{30, 30, 30, 255}, CompositeOp::Normal);
        l->setSpanEnd(0, kOpenEnd);
    }
    doc.setAutoKeyEnabled(true);
    doc.keyCurrentLayer(0);

    TimelineDock dock(&doc);
    dock.resize(900, 300);
    dock.show();

    // Qt reports the leak through qWarning, so capture the messages: with the
    // missing restore() this paints 40x and collects dozens of "Painter ended
    // with 1 saved states" lines, and the test then FAILS. A silent pass is the
    // only way through.
    static QStringList warnings;
    warnings.clear();
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
        if (type == QtWarningMsg && msg.contains(QStringLiteral("saved states")))
            warnings << msg;
    });

    QPixmap pm(dock.size().expandedTo(QSize(400, 300)));
    pm.fill(Qt::transparent);
    for (int i = 0; i < 40; ++i) {
        dock.refresh();
        dock.grid()->update();
        pm.fill(Qt::transparent);
        dock.render(&pm);
        QApplication::processEvents();
    }
    qInstallMessageHandler(nullptr);
    if (!warnings.isEmpty())
        std::printf("QPainter leaked %d state(s); first: %s\n", int(warnings.size()),
                    qPrintable(warnings.first()));
    INK_CHECK(warnings.isEmpty());
    std::printf("painted the timeline 40x with no QPainter state warning\n");
    std::printf("test_painter_state PASS\n");
    return 0;
}
