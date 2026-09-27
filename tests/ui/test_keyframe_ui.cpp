// The keyframe surface, driven headlessly: the Document ops the header buttons
// call, the timeline rows those keys produce, and the curve editor pane.
// Painting is exercised for real (widget.render() into a pixmap), because that
// is the code that has no other way to fail loudly.
#include "../test_check.h"
#include "ui/InkCurveEditor.h"
#include "ui/InkParamFormat.h"
#include "ui/InkDocument.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPixmap>
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

// Paint a widget into a pixmap: a real paint pass over offscreen raster.
QPixmap renderWidget(QWidget &w) {
    QPixmap pm(w.size().expandedTo(QSize(320, 240)));
    pm.fill(Qt::transparent);
    w.render(&pm);
    return pm;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 160, H = 160;

    // 1. The header button's model half: keying a paint layer creates and links
    //    the adjustment layer, and keys the default transform set.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        INK_CHECK(!layer->animated());

        const int n = doc.keyCurrentLayer(0);
        INK_CHECK(n > 0);
        AdjustmentLayer *a = doc.adjustmentFor(layer);
        INK_CHECK(a != nullptr);
        for (ParamId id : defaultKeySet()) INK_CHECK(a->params().hasKey(id, 0));
        INK_CHECK(doc.currentLayer() == layer); // the working layer stays the paint one

        // Keying again at a later frame adds keys there and nothing at home.
        doc.keyCurrentLayer(10);
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 10));
        INK_CHECK(!a->params().hasKey(ParamId::Opacity, 5));

        // Prev/next navigate the union of every parameter's keys.
        doc.setCurrentFrame(5);
        INK_CHECK(doc.prevKeyCurrentLayer(5) == 0);
        INK_CHECK(doc.nextKeyCurrentLayer(5) == 10);
        INK_CHECK(doc.prevKeyCurrentLayer(0) == -1);
        INK_CHECK(doc.nextKeyCurrentLayer(10) == -1);

        // Removing at the playhead clears that frame's keys only.
        INK_CHECK(doc.removeKeyCurrentLayer(0) > 0);
        INK_CHECK(!a->params().hasKey(ParamId::PositionX, 0));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 10));
        std::printf("document key ops OK\n");
    }

    // 2. A key captures the value the parameter ALREADY had, so turning
    //    animation on never moves the picture; and presets do what they say.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a);
        a->params().setStaticValue(ParamId::Rotation, 45.0);

        doc.addParamKey(a, ParamId::Rotation, 3);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Rotation, 3.0) - 45.0) < 1e-9);
        INK_CHECK(a->params().hasKey(ParamId::Rotation, 3));
        INK_CHECK(a->params().keys(ParamId::Rotation).size() == 1);

        // Stopwatch off: the value stays, the animation goes.
        doc.toggleParamKey(a, ParamId::Rotation, 3);
        INK_CHECK(!a->params().isAnimated(ParamId::Rotation));

        // Linear vs Hold vs Easy Ease change the evaluated curve, not the keys.
        doc.setParamStaticValue(a, ParamId::PositionX, 0.0);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 10, 100.0, Interp::Linear);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 5.0) - 50.0) < 1e-3);
        // Easy Ease on both keys: symmetric, still through the midpoint, but
        // slow at both ends — the difference between "it moved" and "it settled".
        doc.applyKeyPreset(a, 0, Interp::Bezier, true, true);
        doc.applyKeyPreset(a, 10, Interp::Bezier, true, true);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 5.0) - 50.0) < 1e-3);
        INK_CHECK(a->params().valueAt(ParamId::PositionX, 1.0) < 10.0);
        INK_CHECK(a->params().valueAt(ParamId::PositionX, 9.0) > 90.0);
        // Easy Ease In only (ease the arrival at the LAST key) is asymmetric.
        doc.applyKeyPreset(a, 10, Interp::Bezier, false, false);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 5.0) - 50.0)
                  > 1.0); // the chord tangent at key 10 changes the shape
        doc.applyKeyPreset(a, 0, Interp::Hold, false, false);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 9.0)) < 1e-6);
        std::printf("key semantics OK\n");
    }

    // 3. Clearing the layer's keyframes (the old "delete adjustment layer")
    //    takes the animation with it; undo brings the keys back.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::ScaleX, 0, 2.0, Interp::Linear);

        PaintDevice withAdj;
        doc.image().project(0, withAdj);
        const int scaledW = withAdj.extent().w;
        INK_CHECK(scaledW > W / 2);

        INK_CHECK(doc.deleteAdjustment(a)); // clears the layer's keyframes
        INK_CHECK(!layer->animated());
        PaintDevice plain;
        doc.image().project(0, plain);
        INK_CHECK(plain.extent().w < scaledW);

        doc.undoEdit();
        INK_CHECK(layer->animated());
        INK_CHECK(layer->params().hasKey(ParamId::ScaleX, 0));
        INK_CHECK(std::abs(layer->params().valueAt(ParamId::ScaleX, 0.0) - 2.0) < 1e-6);
        std::printf("clear keys + undo OK\n");
    }

    // 4. An adjustment layer is ONE row, always. Keying parameters must not grow
    //    the timeline: the properties live in the transform panel instead, so a
    //    layer with eighteen animated parameters costs the same height as one.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        TimelineDock dock(&doc);
        dock.resize(900, 620);
        INK_CHECK(dock.grid() != nullptr);

        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        dock.grid()->refresh();
        const int plain = dock.grid()->contentHeight();

        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 6, 40.0, Interp::Linear);
        doc.setParamKey(a, ParamId::ScaleX, 0, 1.0, Interp::Linear);
        INK_CHECK(a->params().animatedParams().size() == 2);

        TimelineWidget *grid = dock.grid();
        grid->refresh();
        const int keyed = grid->contentHeight();
        // Two animated parameters, three keys: the row height is unchanged.
        INK_CHECK(keyed == plain);

        // Keying every remaining transform parameter still adds no rows.
        for (int i = 0; i < kParamCount; ++i) {
            const ParamId id = (ParamId)i;
            if (paramDesc(id).group != ParamGroup::Transform) continue;
            doc.setParamKey(a, id, 3, a->params().valueAt(id, 3.0), Interp::Linear);
        }
        grid->refresh();
        INK_CHECK(grid->contentHeight() == plain);

        // The keyframe ops through the dock's own paths do not crash and keep
        // the grid consistent.
        grid->noteAnimationChanged();
        grid->refresh();
        INK_CHECK(grid->keySelection().isEmpty());
        dock.refreshContent();
        dock.refresh();

        // Painting the whole dock (grid + curve editor + toolbar) must survive.
        const QPixmap shot = renderWidget(dock);
        INK_CHECK(!shot.isNull());
        std::printf("timeline rows + dock paint OK\n");
    }

    // 5. The easing graph: it lists only the parameters that CAN be eased,
    //    picks the segment under the playhead, and paints both its live and empty
    //    states.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Bezier);
        doc.setParamKey(a, ParamId::PositionX, 12, 120.0, Interp::Bezier);
        // Rotation gets a single key: nothing to ease, so it must NOT be offered.
        doc.setParamKey(a, ParamId::Rotation, 0, 0.0, Interp::Linear);
        doc.setCurrentFrame(6);

        CurveEditor editor(&doc);
        editor.resize(360, 340);
        editor.setAdjustment(a);
        INK_CHECK(editor.adjustment() == a);
        // The dropdown offers one entry per transform GROUP — never separate
        // X/Y entries. Seven groups: Anchor, Position, Scale, Rotation, Shear,
        // Perspective, Opacity.
        INK_CHECK(editor.groupCount() == 7);
        INK_CHECK(editor.groupLabel(0) == QStringLiteral("Anchor"));
        INK_CHECK(editor.groupLabel(1) == QStringLiteral("Position"));
        INK_CHECK(editor.groupLabel(2) == QStringLiteral("Scale"));
        INK_CHECK(editor.groupLabel(3) == QStringLiteral("Rotation"));
        INK_CHECK(editor.groupLabel(4) == QStringLiteral("Shear"));
        INK_CHECK(editor.groupLabel(5) == QStringLiteral("Perspective"));
        INK_CHECK(editor.groupLabel(6) == QStringLiteral("Opacity"));
        INK_CHECK(!editor.dropdownRect().isEmpty());
        // Position has the only real segment: the graph opens on it.
        INK_CHECK(editor.currentGroup() == 1);
        INK_CHECK(editor.channel() == ParamId::PositionX);
        INK_CHECK(editor.hasSegment());
        INK_CHECK(editor.frameA() == 0 && editor.frameB() == 12);
        INK_CHECK(editor.segmentIndex() == 0 && editor.segmentCount() == 1);
        INK_CHECK(!editor.flat());
        // Untouched auto handles sit at the neutral third: a straight line.
        INK_CHECK(std::abs(editor.normA().x() - 1.0 / 3.0) < 1e-3);
        INK_CHECK(std::abs(editor.normB().x() - 2.0 / 3.0) < 1e-3);
        // A straight shape is NOT a linear mode: this segment is Bezier with
        // auto tangents, so its handles are live and nothing is highlighted.
        // Lighting "Linear" here would tell the user dragging does nothing.
        INK_CHECK(editor.presetHighlight() == -1);
        doc.setParamKey(a, ParamId::ScaleX, 0, 1.0, Interp::Linear);
        doc.setParamKey(a, ParamId::ScaleX, 12, 2.0, Interp::Linear);
        editor.refresh();
        // Picking a group from the dropdown opens ITS curve: Scale has the
        // linear segment, so the mode and preset highlight follow the choice.
        editor.selectGroup(2);
        INK_CHECK(editor.currentGroup() == 2);
        INK_CHECK(editor.channel() == ParamId::ScaleX);
        INK_CHECK(editor.hasSegment());
        INK_CHECK(editor.mode() == Interp::Linear);
        INK_CHECK(editor.presetHighlight() == 0); // a real Linear segment lights Linear
        INK_CHECK(!renderWidget(editor).isNull());
        // Rotation has a single key: the group is still selectable, it just
        // has no segment to ease yet (the hint explains that).
        editor.selectGroup(3);
        INK_CHECK(editor.currentGroup() == 3);
        INK_CHECK(editor.channel() == ParamId::Rotation);
        INK_CHECK(!editor.hasSegment());
        INK_CHECK(!renderWidget(editor).isNull());
        editor.selectGroup(1); // back to Position for the later assertions

        // Clearing the layer leaves the graph empty and it must still paint.
        editor.setAdjustment(nullptr);
        INK_CHECK(!editor.hasSegment());
        INK_CHECK(!renderWidget(editor).isNull());
        std::printf("easing graph OK\n");
    }

    // ------------------------------------------------------------------
    // 5c. The graph's header spells its endpoints with the SAME formatter the
    //     transform panel's fields use, so a scale cannot read "1.0" in one pane
    //     and "100%" in the other. Compared against formatParamValue itself
    //     rather than a hardcoded string, so the two can never drift silently.
    {
        Document doc(W, H, 24);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::ScaleX, 0, 1.0, Interp::Bezier);
        doc.setParamKey(a, ParamId::ScaleX, 12, 1.6, Interp::Bezier);
        doc.setCurrentFrame(6);

        CurveEditor ed(&doc);
        ed.setAdjustment(a);
        INK_CHECK(ed.hasSegment());
        INK_CHECK(ed.channel() == ParamId::ScaleX);
        const QString hdr = ed.statusText();
        INK_CHECK(hdr.contains(formatParamValue(ParamId::ScaleX, 1.0)));
        INK_CHECK(hdr.contains(formatParamValue(ParamId::ScaleX, 1.6)));
        // Rounded values print WITHOUT padding ("100%", not "100.0%"), while a
        // value that needs decimals keeps them: precision is for the digits that
        // matter, not fixed padding in a narrow field.
        INK_CHECK(formatParamValue(ParamId::ScaleX, 1.0) == QStringLiteral("100%"));
        INK_CHECK(formatParamValue(ParamId::ScaleX, 1.6) == QStringLiteral("160%"));
        INK_CHECK(formatParamValue(ParamId::Opacity, 0.004) == QStringLiteral("0.4%"));
        INK_CHECK(formatParamValue(ParamId::PositionX, 10.0) == QStringLiteral("10"));
        INK_CHECK(formatParamValue(ParamId::Rotation, 0.125)
                  == QString::fromUtf8("0.125\xC2\xB0"));
        // And a bare typed number means the same thing the readout showed.
        bool pOk = false;
        INK_CHECK(qFuzzyCompare(parseParamValue(ParamId::ScaleX, "160", &pOk), 1.6) && pOk);
        std::printf("graph header formatting OK: %s\n", qPrintable(hdr));
    }

    // 6. The graph is INTERACTIVE and edits the real model: a preset thumbnail
    //    changes what the compositor evaluates, dragging a handle rewrites the
    //    stored handles, and each gesture is exactly one undo step.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Bezier);
        doc.setParamKey(a, ParamId::PositionX, 10, 100.0, Interp::Bezier);
        doc.setCurrentFrame(5);

        CurveEditor editor(&doc);
        editor.resize(360, 340);
        editor.setAdjustment(a);
        INK_CHECK(editor.hasSegment());

        // (a) Presets applied to the frame flatten the segment (the thumbnail
        //     column was removed — presets live in the key menu now — but the
        //     model path they drive is unchanged): Hold at frame 5, one undo
        //     brings the curve back.
        INK_CHECK(editor.presetCell(0).isEmpty()); // the column is really gone
        const double beforeHold = a->params().valueAt(ParamId::PositionX, 8.0);
        const size_t depthA = doc.undo().undoDepth();
        doc.applyKeyPreset(a, editor.frameA(), Interp::Hold, false, false);
        // Hold: the segment sits at key A's value until key B arrives.
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 8.0)) < 1e-6);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 8.0) - beforeHold) > 1e-6);
        INK_CHECK(doc.undo().undoDepth() == depthA + 1); // ONE step for the preset
        doc.undoEdit();
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 8.0) - beforeHold) < 1e-6);
        editor.refresh();

        // (b) Dragging a handle changes the stored influence/speed AND the
        //     evaluated curve — the drawing is not decorative.
        const ScalarChannel *ch = a->params().channel(ParamId::PositionX);
        const double inflBefore = double(ch->keyAt(0)->outInfl);
        const double speedBefore = double(ch->keyAt(0)->outSpeed);
        const double midBefore = a->params().valueAt(ParamId::PositionX, 5.0);
        const QPoint ha(editor.handlePointA().x(), editor.handlePointA().y());
        INK_CHECK(editor.handleRectA().contains(ha));
        const size_t depthB = doc.undo().undoDepth();
        {
            QMouseEvent press(QEvent::MouseButtonPress, ha, editor.mapToGlobal(ha), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&editor, &press);
            // Pull the handle right and down towards the bottom-right corner:
            // a strong ease-in, so the midpoint must fall below the straight line.
            const QPoint to = ha + QPoint(60, 60);
            QMouseEvent move(QEvent::MouseMove, to, editor.mapToGlobal(to), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&editor, &move);
            QMouseEvent rel(QEvent::MouseButtonRelease, to, editor.mapToGlobal(to), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&editor, &rel);
        }
        ch = a->params().channel(ParamId::PositionX);
        INK_CHECK(std::abs(double(ch->keyAt(0)->outInfl) - inflBefore) > 1e-4);
        INK_CHECK(std::abs(double(ch->keyAt(0)->outSpeed) - speedBefore) > 1e-4);
        const double midAfter = a->params().valueAt(ParamId::PositionX, 5.0);
        INK_CHECK(midAfter < midBefore); // ease-in lags behind linear at the middle
        INK_CHECK(doc.undo().undoDepth() == depthB + 1);
        doc.undoEdit();
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 5.0) - midBefore) < 1e-6);
        editor.refresh();

        // (c) The normalised points round-trip: what the panel reads back is what
        //     Document stored, so a redrawn handle cannot drift.
        {
            const QPoint hb(int(editor.handlePointB().x()), int(editor.handlePointB().y()));
            QMouseEvent press(QEvent::MouseButtonPress, hb, editor.mapToGlobal(hb), Qt::LeftButton,
                              Qt::LeftButton, Qt::AltModifier); // Alt = broken tangent
            QApplication::sendEvent(&editor, &press);
            const QPoint to = hb + QPoint(-40, 30);
            QMouseEvent move(QEvent::MouseMove, to, editor.mapToGlobal(to), Qt::NoButton,
                             Qt::LeftButton, Qt::AltModifier);
            QApplication::sendEvent(&editor, &move);
            QMouseEvent rel(QEvent::MouseButtonRelease, to, editor.mapToGlobal(to), Qt::LeftButton,
                            Qt::NoButton, Qt::AltModifier);
            QApplication::sendEvent(&editor, &rel);
        }
        const QPointF nb = editor.normB();
        const ScalarKey *kb = a->params().channel(ParamId::PositionX)->keyAt(10);
        INK_CHECK(std::abs((1.0 - double(kb->inInfl)) - nb.x()) < 1e-3);
        // A broken tangent leaves key B's OUT side exactly where it was (neutral).
        INK_CHECK(std::abs(double(kb->outInfl) - 1.0 / 3.0) < 1e-3);
        doc.undoEdit();
        editor.refresh();

        // (c2) The normalised round-trip is EXACT in both directions. The B
        //      handle is measured back from key B, so an inverse that forgets the
        //      (1 - y) reads an ease as its own mirror image — this pins that.
        const int fA = editor.frameA();
        const double span = double(editor.frameB() - fA);
        {
            // Drive the Ease In/Out preset through the same model path the key
            // menu uses, then check the graph reads the stored shape back.
            doc.applyKeyPreset(a, editor.frameA(), Interp::Bezier, true, true);
            const ScalarKey *ka0 = a->params().channel(ParamId::PositionX)->keyAt(editor.frameA());
            const ScalarKey *kb0 = a->params().channel(ParamId::PositionX)->keyAt(editor.frameB());
            INK_CHECK(ka0 && kb0);
            doc.beginParamRecord("easing roundtrip", a);
            doc.liveSetSegmentEase(a, ParamId::PositionX, editor.frameA(), 0.62, 0.02, 0.38, 0.98);
            doc.endParamRecord();
            editor.refresh();
            // The panel reads back what was stored.
            INK_CHECK(std::abs(editor.normA().x() - 0.62) < 1e-3);
            INK_CHECK(std::abs(editor.normA().y() - 0.02) < 1e-3);
            INK_CHECK(std::abs(editor.normB().x() - 0.38) < 1e-3);
            INK_CHECK(std::abs(editor.normB().y() - 0.98) < 1e-3);
            // The shape is real: an ease-in/out is BEHIND linear at the first
            // quarter and AHEAD at the third (it accelerates through the middle).
            const double lin1 = editor.valueA() + 0.25 * (editor.valueB() - editor.valueA());
            const double lin3 = editor.valueA() + 0.75 * (editor.valueB() - editor.valueA());
            const double q1 = a->params().valueAt(ParamId::PositionX, fA + 0.25 * span);
            const double q3 = a->params().valueAt(ParamId::PositionX, fA + 0.75 * span);
            INK_CHECK(q1 < lin1);
            INK_CHECK(q3 > lin3);
            doc.undoEdit();
            editor.refresh();
        }

        // (d) Paging moves the playhead, so the timeline and the graph agree.
        doc.setParamKey(a, ParamId::PositionX, 20, 40.0, Interp::Bezier);
        editor.refresh();
        INK_CHECK(editor.segmentCount() == 2);
        INK_CHECK(editor.frameA() == 0 && editor.frameB() == 10);
        const QRect next = editor.nextSegmentRect();
        {
            QMouseEvent press(QEvent::MouseButtonPress, next.center(), editor.mapToGlobal(next.center()),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&editor, &press);
        }
        INK_CHECK(editor.segmentIndex() == 1);
        INK_CHECK(editor.frameA() == 10 && editor.frameB() == 20);
        INK_CHECK(doc.currentFrame() == 10); // the playhead followed
        std::printf("easing interaction OK\n");
    }

    std::printf("test_keyframe_ui PASS\n");
    return 0;
}
