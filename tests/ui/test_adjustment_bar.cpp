// Keyframes live ON the paint layer's own lane: the range rect the widget
// exposes, the retiming drag through real mouse events, and the gestures that
// open the transform panel. (The adjustment layer — and with it the bar/span
// concept — is gone; these are the interactions that replaced it.)
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include "ui/InkCurveEditor.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QToolButton>
#include <QPixmap>
#include <cmath>
#include <cstdio>

using namespace ink;

namespace {

PaintLayer *paintSquare(Document &doc, int w, int h) {
    GroupLayer *folder = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(folder, "art");
    PaintDevice *dev = l->deviceAt(0, true);
    dev->fillRect(Rect{w / 4, h / 4, w / 2, h / 2}, PixelRGBA8{30, 30, 30, 255},
                  CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

QPixmap renderWidget(QWidget &w) {
    QPixmap pm(w.size().expandedTo(QSize(320, 240)));
    pm.fill(Qt::transparent);
    w.render(&pm);
    return pm;
}

void sendClick(QWidget *w, const QPoint &p, Qt::MouseButton btn = Qt::LeftButton) {
    QMouseEvent press(QEvent::MouseButtonPress, p, w->mapToGlobal(p), btn, btn,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p, w->mapToGlobal(p), btn,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QApplication::sendEvent(w, &release);
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 160, H = 160;

    // 1. Keys on the layer drive the compositor on EVERY frame — there is no
    //    bar to fall outside of any more.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::ScaleX, 0, 2.0, Interp::Linear);
        doc.setParamKey(a, ParamId::ScaleX, 20, 2.0, Interp::Linear);

        INK_CHECK(a->activeAt(0) && a->activeAt(23)); // active everywhere
        PaintDevice wide;
        doc.image().project(0, wide);
        const int scaledW = wide.extent().w;
        INK_CHECK(scaledW > 120); // > the bare square's tile-granular extent
        PaintDevice at12;
        doc.image().project(12, at12);
        INK_CHECK(at12.extent().w == scaledW); // mid-clip: still scaled
        PaintDevice at23;
        doc.image().project(23, at23);
        INK_CHECK(at23.extent().w == scaledW); // past the keys: held value
        std::printf("keys live everywhere OK\n");
    }

    // 2. The preview/commit retime session: preview is idempotent (rebased on
    //    the snapshot every move), commit is one undo step, abort restores.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 4, 0.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 8, 40.0, Interp::Linear);

        const ParamBlock base = a->params();

        // Three preview moves; the last one wins, nothing compounds.
        doc.previewAdjustmentBar(a, 6, 10, +2, base);
        doc.previewAdjustmentBar(a, 9, 13, +5, base);
        doc.previewAdjustmentBar(a, 7, 11, +3, base);
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 7));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 11));
        INK_CHECK(!a->params().hasKey(ParamId::PositionX, 4));

        // Rebase from the snapshot: the keys are back at their ORIGINAL frames.
        doc.previewAdjustmentBar(a, 9, 11, 0, base);
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 4));
        INK_CHECK(!a->params().hasKey(ParamId::PositionX, 7));

        // Commit the move: one undo restores the keys together.
        doc.previewAdjustmentBar(a, 7, 11, +3, base);
        doc.commitAdjustmentBar(a, 4, 8, base);
        doc.undoEdit();
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 4));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 8));
        doc.redoEdit();
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 7));
        std::printf("retime session OK\n");
    }

    // 3. Timeline widget: the dock builds/refreshes/paints with keyed layers.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        TimelineDock dock(&doc);
        dock.resize(900, 620);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 5, 0.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 9, 30.0, Interp::Linear);

        TimelineWidget *grid = dock.grid();
        grid->refresh();
        dock.refreshContent();
        const QPixmap shot = renderWidget(dock);
        INK_CHECK(!shot.isNull());
        std::printf("timeline key paint OK\n");
    }

    // 4. A keyed layer's range rect spans first..last key and stays on screen.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 4, 10.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 8, 40.0, Interp::Linear);

        TimelineDock dock(&doc);
        dock.resize(900, 620);
        auto *grid = dock.grid();
        grid->resize(860, 560);
        grid->refresh();
        const QString id = QString::fromStdString(layer->stableId());
        const QRect range = grid->keyedRangeRect(id);
        INK_CHECK(!range.isEmpty());
        INK_CHECK(range.right() < grid->width()); // reachable, not off-screen
        INK_CHECK(range.right() > range.left() + 20);
        // A layer with no keys has no range.
        PaintLayer *bare = doc.createTrack(doc.ensureFolder(), "bare");
        grid->refresh();
        INK_CHECK(grid->keyedRangeRect(QString::fromStdString(bare->stableId())).isEmpty());
        std::printf("keyed range rect OK\n");
    }

    // 5. The keyed range is NOT a transform surface any more. Double-clicking
    //    or right-clicking it used to pop a second, divergent transform panel
    //    (and the right-click hijack swallowed the ordinary cell menu). Now the
    //    toolbar transform and the easing graph are the only two surfaces, and
    //    both these gestures fall through to the normal timeline behaviour.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Linear);
        // A second key so the range has body BETWEEN its diamonds: a click at
        // the range centre is a range gesture, not a keyframe-menu gesture.
        doc.setParamKey(a, ParamId::PositionX, 12, 60.0, Interp::Linear);
        doc.setCurrentFrame(0);

        TimelineDock dock(&doc);
        dock.resize(900, 620);
        auto *grid = dock.grid();
        grid->refresh();

        // The keyed range still reports its geometry (the drag-to-retime
        // gesture below depends on it)...
        const QRect range =
            grid->keyedRangeRect(QString::fromStdString(layer->stableId()));
        INK_CHECK(!range.isEmpty());
        const QPoint hit = range.center();

        // ...but double-clicking it opens no transform panel: the dock has no
        // such surface to open, and the click does not disturb the model.
        const size_t depth = doc.undo().undoDepth();
        QMouseEvent dbl(QEvent::MouseButtonDblClick, hit, grid->mapToGlobal(hit),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grid, &dbl);
        INK_CHECK(doc.undo().undoDepth() == depth);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0)) < 1e-6);
        std::printf("keyed range opens no transform panel OK\n");

        // The same for a right-click, asserted on the SIGNAL the grid emits
        // rather than by letting the menu open: the dock turns it into a modal
        // exec(), which a headless run cannot dismiss. What matters is that the
        // keyed range routes to the ordinary cell menu instead of being
        // hijacked into a transform surface.
        int laneMenus = 0;
        // Unhook every consumer FIRST so the dock's modal exec() never runs —
        // we assert on the signal itself, and a headless run cannot dismiss a
        // real menu. Re-add our own counter after.
        QObject::disconnect(grid, &TimelineWidget::laneMenuRequested, nullptr, nullptr);
        QObject::connect(grid, &TimelineWidget::laneMenuRequested, grid,
                         [&](const QPoint &, const QString &, int) { ++laneMenus; });
        QMouseEvent rcP(QEvent::MouseButtonPress, hit, grid->mapToGlobal(hit),
                        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QMouseEvent rcR(QEvent::MouseButtonRelease, hit, grid->mapToGlobal(hit),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(grid, &rcP);
        QApplication::sendEvent(grid, &rcR);
        INK_CHECK(laneMenus == 1); // the NORMAL cell menu, reached
        INK_CHECK(doc.undo().undoDepth() == depth);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0)) < 1e-6);
        std::printf("keyed range right-click reaches the normal menu OK\n");
    }

    // 6. The keys DRAGGED THROUGH THE WIDGET: grabbing the keyed range body
    //    retimes every key by exactly the pointer delta, keys travelling with
    //    it, as ONE undo step.
    {
        Document doc(W, H, 24);
        doc.setFrameCount(24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 4, 10.0, Interp::Linear);
        doc.setCurrentFrame(0);

        TimelineDock dock(&doc);
        dock.resize(900, 620);
        auto *grid = dock.grid();
        grid->resize(860, 560);
        grid->refresh();
        const QString id = QString::fromStdString(layer->stableId());
        QRect range = grid->keyedRangeRect(id);
        INK_CHECK(!range.isEmpty());
        INK_CHECK(range.right() < grid->width());

        auto drag = [&](const QPoint &from, const QPoint &to) {
            QMouseEvent p(QEvent::MouseButtonPress, from, grid->mapToGlobal(from),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent m(QEvent::MouseMove, to, grid->mapToGlobal(to), Qt::NoButton,
                          Qt::LeftButton, Qt::NoModifier);
            QMouseEvent r(QEvent::MouseButtonRelease, to, grid->mapToGlobal(to),
                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(grid, &p);
            QApplication::sendEvent(grid, &m);
            QApplication::sendEvent(grid, &r);
        };

        // Grab the middle and move one frame right: the key travels exactly one
        // frame (a grab-offset bug would snap it under the cursor instead).
        const QPoint mid = range.center();
        drag(mid, mid + QPoint(int(grid->pxPerFrame()), 0));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 5));
        INK_CHECK(!a->params().hasKey(ParamId::PositionX, 4));
        doc.undoEdit();
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 4));
        std::printf("range body drag no-snap OK\n");

        // A do-nothing press on the range must not create an undo step.
        const size_t depth = doc.undo().undoDepth();
        range = grid->keyedRangeRect(id);
        sendClick(grid, range.center());
        INK_CHECK(doc.undo().undoDepth() == depth);
        std::printf("range click no-op OK\n");
    }

    // 7. A diamond is clickable WHEREVER it is painted: clicking a key on the
    //    layer's lane selects every parameter keyed there.
    {
        Document doc(W, H, 24);
        doc.setFrameCount(24);
        PaintLayer *layer = paintSquare(doc, W, H);
        // Drop the boot "Layer 1" so this row is the FIRST track row: the click
        // coordinates below are computed from the row layout, not searched.
        {
            std::vector<PaintLayer*> all;
            doc.image().collectPaintLayers(all);
            for (PaintLayer *p : all)
                if (p != layer) doc.deleteTrack(p);
        }
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 2, 10.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 8, 40.0, Interp::Linear);
        doc.setCurrentFrame(0);

        TimelineDock dock(&doc);
        dock.resize(900, 620);
        auto *grid = dock.grid();
        grid->resize(860, 560);
        grid->refresh();

        // The x of the key at frame 2, on its row's vertical centre.
        const auto rows = [&] {
            return grid->contentHeight() > 0;
        };
        INK_CHECK(rows());
        const int xKey2 = int(grid->pxPerFrame()) * 2 + InkTheme::gutterW() +
                          int(grid->pxPerFrame() / 2);
        const int yRow = InkTheme::rulerH() + InkTheme::folderH() +
                         InkTheme::trackH() / 2;
        const QPoint at(xKey2, yRow);
        sendClick(grid, at);
        bool gotKey = false;
        for (const TimelineKey &k : grid->keySelection())
            if (k.frame == 2 && k.param == int(ParamId::PositionX)) gotKey = true;
        INK_CHECK(gotKey);
        std::printf("lane diamond clickable OK\n");
    }

    std::printf("test_adjustment_bar PASS\n");
    return 0;
}
