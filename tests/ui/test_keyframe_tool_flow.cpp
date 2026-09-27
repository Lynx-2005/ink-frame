// Regressions for the reported "keyframe mode" complaints, all of which used to
// ship because nothing covered the interaction between a keyframe and the
// ordinary canvas tools:
//
//   1. The easing graph could not drag a handle to the edge of its box (x was
//      clamped to 0.01..0.99, so the handle stopped short of both rails).
//   2. Recording is OPT-IN ("Record as keyframes" toggle, default off): with
//      the toggle off, Transform/Move always bake pixels — even on an animated
//      layer — and the status hint says where the toggle lives. With it on, a
//      drag writes keys and leaves the pixels alone. (An earlier implicit
//      takeover made every drag on a keyed layer write keys unasked, which is
//      what made "add a keyframe, then transform" feel broken.)
//   3. Same contract for the move tool.
//   4. The keyframe right-click menu offered a separate "Transform properties"
//      panel — a second, divergent transform surface. The toolbar transform is
//      the only one.
//   5. A recorded flip/rotate (one-shot op) closes its own undo step instead
//      of leaking the open record into the next drag.
//   6. Scrubbing an animated channel's value field with Auto-key on keys the
//      frame instead of writing a static value valueAt() would ignore.
//
// The lasso half is the control case: adding keyframes must NOT disturb it, so
// a lasso drawn over keyframed ink still selects the ink it circles.
//
// The lasso half is the control case: adding keyframes must NOT disturb it, so
// a lasso drawn over keyframed ink still selects the ink it circles.
#include "../test_check.h"
#include "ui/InkCurveEditor.h"
#include "ui/InkDocument.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTransformPanel.h" // ParamField (value-field scrub test)
#include "ui/tools/InkMoveTool.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QStringList>
#include <cstdio>
#include <vector>

using namespace ink;

namespace {

struct NullHost : ToolHost {
    void repaint() override {}
    void rebuild() override {}
};

ToolEvent ev(double x, double y) {
    ToolEvent e;
    e.docPos = {x, y};
    e.zoom = 1.0;
    return e;
}

std::vector<uint8_t> snapshot(const PaintDevice &dev, int w, int h) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    const_cast<PaintDevice &>(dev).readRect({0, 0, w, h}, buf.data());
    return buf;
}

// Fraction of differing bytes between two snapshots (0 = identical).
double diffFraction(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++diff;
    return double(diff) / double(a.size());
}

// A layer with a solid square, exposed from frame 0 onwards.
PaintLayer *paintSquare(Document &doc, int w, int h) {
    GroupLayer *folder = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(folder, "art");
    PaintDevice *dev = l->deviceAt(0, true);
    dev->fillRect(Rect{w / 4, h / 4, w / 2, h / 2}, PixelRGBA8{40, 90, 200, 255},
                  CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

void drag(QWidget &w, const QPoint &from, const QPoint &to) {
    QMouseEvent press(QEvent::MouseButtonPress, from, w.mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &press);
    QMouseEvent move(QEvent::MouseMove, to, w.mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&w, &move);
    QMouseEvent rel(QEvent::MouseButtonRelease, to, w.mapToGlobal(to), Qt::LeftButton,
                    Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &rel);
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 200, H = 200;

    // 1. The graph handle reaches the box edge.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Bezier);
        doc.setParamKey(a, ParamId::PositionX, 10, 100.0, Interp::Bezier);

        CurveEditor editor(&doc);
        editor.resize(320, 260);
        editor.setAdjustment(a);
        editor.refresh();
        INK_CHECK(editor.hasSegment());

        // Drag handle A onto the RIGHT rail of the box. A used to stop at 0.99,
        // leaving a visible gap between the handle and the edge.
        drag(editor, editor.handlePointA().toPoint(),
             QPoint(editor.width(), editor.handlePointA().toPoint().y())); // far right
        INK_CHECK(editor.normA().x() > 0.985);

        // ...and handle B onto the LEFT rail (x = 0, tucked against the end key).
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Bezier);
        doc.setParamKey(a, ParamId::PositionX, 10, 100.0, Interp::Bezier);
        editor.refresh();
        drag(editor, editor.handlePointB().toPoint(), QPoint(0, editor.handlePointB().toPoint().y()));
        INK_CHECK(editor.normB().x() < 0.015);

        // The stored handles agree with what the handle shows, so a refresh
        // cannot snap it back inwards.
        editor.refresh();
        INK_CHECK(editor.normB().x() < 0.02);
        std::printf("graph handle reaches the box edge OK\n");
    }

    // 2. The toolbar transform drives keyframes on an animated layer once the
    //    RECORD toggle is on: pixels untouched, a key written.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0); // "add a keyframe"
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().keyCount() > 0);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // opt-in: the RECORD toggle
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        // In record mode the frame outlines the CANVAS, so its pivot grip sits
        // at the canvas centre (100,100). Grab the body well away from it.
        tool.beginPrimary(ev(60, 60));
        tool.continuePrimary(ev(90, 60));
        tool.endPrimary(ev(90, 60));

        // The move lands in the ANIMATION, not in the cel.
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 30.0) < 1.5);
        // And the composite really shows it.
        PaintDevice frame;
        doc.image().project(0, frame);
        INK_CHECK(!frame.extent().isEmpty());
        std::printf("transform tool records on an animated layer OK\n");
    }

    // 2b. The other half of the contract: with RECORD off, the same drag on
    //     the same animated layer bakes PIXELS (never hijacks into keys), and
    //     the hint points at the toggle.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().keyCount() > 0);
        const int keysBefore = a->params().keyCount();

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx); // RECORD deliberately left OFF
        INK_CHECK(tool.statusHint().contains(QStringLiteral("RECORD")));
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(110, 80));
        tool.endPrimary(ev(110, 80));
        INK_CHECK(before != snapshot(*layer->deviceAt(0, false), W, H)); // pixels moved
        INK_CHECK(a->params().keyCount() == keysBefore);                 // no new keys
        std::printf("toggle-off bakes on an animated layer OK\n");
    }

    // 3. A layer with NO keyframes still bakes pixels: the takeover is
    //    conditional, so ordinary transforming is unchanged.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        INK_CHECK(!doc.ensureAdjustmentFor(layer)->params().isAnimated(ParamId::PositionX));

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(110, 80));
        tool.endPrimary(ev(110, 80));
        INK_CHECK(before != snapshot(*layer->deviceAt(0, false), W, H)); // pixels moved
        std::printf("unkeyed layer still bakes OK\n");
    }

    // 4. Same for the move tool (opt-in on both sides).
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(doc.selection().isEmpty()); // whole-layer move

        MoveTool tool;
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(70, 70));
        tool.continuePrimary(ev(95, 70));
        tool.endPrimary(ev(95, 70));
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 25.0) < 0.6);
        std::printf("move tool records on an animated layer OK\n");
    }

    // 4b. Move with RECORD off on an animated layer: pixels bake, hint shows.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        const int keysBefore = a->params().keyCount();

        MoveTool tool;
        tool.activate(ctx); // RECORD deliberately left OFF
        INK_CHECK(tool.statusHint().contains(QStringLiteral("RECORD")));
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(70, 70));
        tool.continuePrimary(ev(95, 70));
        tool.endPrimary(ev(95, 70));
        INK_CHECK(before != snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(a->params().keyCount() == keysBefore);
        std::printf("move toggle-off bakes on an animated layer OK\n");
    }

    // 4c. A recorded one-shot op (flip) closes its own undo step: the keys it
    //     wrote must not leak into the next drag's record.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const size_t depth = doc.undo().undoDepth();
        tool.flipSelection(true);
        INK_CHECK(!tool.isEditing());
        INK_CHECK(doc.undo().undoDepth() == depth + 1); // flip landed as one step
        INK_CHECK(a->params().isAnimated(ParamId::ScaleX));
        // A following drag is a SEPARATE undo step, not a continuation.
        tool.beginPrimary(ev(60, 60));
        tool.continuePrimary(ev(90, 60));
        tool.endPrimary(ev(90, 60));
        tool.commit(); // explicit Apply: the release only previewed
        INK_CHECK(doc.undo().undoDepth() == depth + 2);
        doc.undoEdit();
        INK_CHECK(doc.undo().undoDepth() == depth + 1);
        INK_CHECK(a->params().isAnimated(ParamId::ScaleX)); // flip's key survived
        std::printf("recorded flip closes its undo step OK\n");
    }

    // 4d. Scrubbing an animated channel's value field with Auto-key on keys
    //     the frame (it used to write a static valueAt() ignores = dead scrub).
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(5);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Linear);
        doc.setParamKey(a, ParamId::PositionX, 10, 100.0, Interp::Linear);
        INK_CHECK(!a->params().hasKey(ParamId::PositionX, 5));

        CurveEditor editor(&doc);
        editor.resize(420, 260);
        editor.setAdjustment(a);
        // Open the Position group so its value fields exist.
        int posGroup = -1;
        for (int g = 0; g < editor.groupCount(); ++g)
            if (editor.groupLabel(g).contains(QStringLiteral("Position"))) posGroup = g;
        INK_CHECK(posGroup >= 0);
        editor.selectGroup(posGroup);
        editor.refresh();
        ParamField *f = editor.valueField(ParamId::PositionX);
        INK_CHECK(f != nullptr);
        // A real press-drag-release on the field: press-and-move scrubs.
        const QPoint p0(10, 5), p1(70, 5);
        QMouseEvent press(QEvent::MouseButtonPress, p0, f->mapToGlobal(p0), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(f, &press);
        QMouseEvent move(QEvent::MouseMove, p1, f->mapToGlobal(p1), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(f, &move);
        QMouseEvent rel(QEvent::MouseButtonRelease, p1, f->mapToGlobal(p1), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(f, &rel);
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 5));
        // The linear mid-value would be 50: the key holds the scrubbed value.
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 5.0) - 50.0) > 0.5);
        std::printf("animated scrub keys the frame OK\n");
    }

    // 4e. THE ball scenario: one cell over frames 0..9, Add-key at both ends,
    //     perspective drag at 9 with RECORD on — the middle must interpolate.
    //     (Add-key used to skip corners entirely, so the end key held backwards
    //     and nothing moved.)
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setAutoKeyEnabled(true);
        layer->setSpanEnd(0, 9);
        doc.setCurrentFrame(0);
        INK_CHECK(doc.keyCurrentLayer(0) == 18); // the whole transform, corners in
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().hasKey(ParamId::Corner0X, 0));
        doc.setCurrentFrame(9);
        INK_CHECK(doc.keyCurrentLayer(9) == 18);
        INK_CHECK(a->params().hasKey(ParamId::Corner0X, 9));

        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        QString lastStatus;
        QObject::connect(&ctx, &ToolContext::statusChanged, &ctx,
                         [&](const QString &s) { lastStatus = s; });
        Vec2 c4e[4];
        tool.frameCornersForTest(c4e); // the content corner, not the canvas one
        tool.beginPrimary(ev(c4e[0].x, c4e[0].y));
        tool.continuePrimary(ev(c4e[0].x - 20, c4e[0].y - 20));
        tool.endPrimary(ev(c4e[0].x - 20, c4e[0].y - 20));
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H)); // pixels stay
        // Frame-relative: the content TL (50,50) moved to (30,30) against the
        // (50,50,100,100) frame keyed by Add-key.
        const double v9 = a->params().valueAt(ParamId::Corner0X, 9.0);
        INK_CHECK(std::abs(v9 - (-0.2)) < 0.02);
        // Mid-span interpolates between the rest corner (0) and the moved one.
        const double v45 = a->params().valueAt(ParamId::Corner0X, 4.5);
        INK_CHECK(v45 < -1e-6 && v45 > v9);
        // Both ends were keyed up front, so this is a move — not a first key.
        INK_CHECK(!lastStatus.contains(QStringLiteral("Add-key")));
        // And the composite really bends mid-span.
        PaintDevice mid;
        doc.image().project(4, mid);
        INK_CHECK(!mid.extent().isEmpty());
        std::printf("perspective animates across the cell OK\n");
    }

    // 4f. Lone end key holds (documents the semantics): without an Add-key at
    //     the start, frames before it show the end pose — static, by design.
    //     The commit says so instead of auto-keying anything.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setAutoKeyEnabled(true);
        layer->setSpanEnd(0, 9);
        doc.setCurrentFrame(9);

        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        QString lastStatus;
        QObject::connect(&ctx, &ToolContext::statusChanged, &ctx,
                         [&](const QString &s) { lastStatus = s; });
        tool.beginPrimary(ev(0, 0));
        tool.continuePrimary(ev(-20, -20));
        tool.endPrimary(ev(-20, -20));
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().hasKey(ParamId::Corner0X, 9));
        INK_CHECK(!a->params().hasKey(ParamId::Corner0X, 0)); // nothing automatic
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Corner0X, 0.0) -
                           a->params().valueAt(ParamId::Corner0X, 9.0)) < 1e-9);
        INK_CHECK(lastStatus.contains(QStringLiteral("Add-key")));
        std::printf("lone end key holds + hint OK\n");
    }

    // 4g. Move's first Position key hints the same way.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(3);
        doc.setAutoKeyEnabled(true);

        MoveTool tool;
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        QString lastStatus;
        QObject::connect(&ctx, &ToolContext::statusChanged, &ctx,
                         [&](const QString &s) { lastStatus = s; });
        tool.beginPrimary(ev(70, 70));
        tool.continuePrimary(ev(95, 70));
        tool.endPrimary(ev(95, 70));
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 3));
        INK_CHECK(lastStatus.contains(QStringLiteral("Add-key")));
        std::printf("move first-key hint OK\n");
    }

    // 5. The lasso still works once keyframes exist: keyframed ink is selected
    //    by circling where it DISPLAYS, and the mask is stored where the pixels
    //    are (nothing baked).
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(a, ParamId::PositionX, 0, 40.0, Interp::Linear);
        // The offset follows the mask's TARGET layer, so the lasso below is what
        // tags it; sync first with the tag set, as a lasso release would.
        doc.selection().setTargetId(layer->id());
        doc.syncSelectionTransform();
        INK_CHECK(doc.selection().offsetActive());

        // The square now DISPLAYS at 90..190; circle it.
        LassoSelectTool lasso;
        lasso.activate(ctx);
        lasso.beginPrimary(ev(80, 80));
        for (int i = 1; i <= 8; ++i) lasso.continuePrimary(ev(80 + i * 8, 80));
        for (int i = 0; i <= 8; ++i) lasso.continuePrimary(ev(150, 80 + i * 8));
        lasso.endPrimary(ev(150, 150));
        INK_CHECK(!doc.selection().isEmpty());
        INK_CHECK(doc.selection().targetId() == layer->id());
        // The plain lasso stores the POLYGON, mapped back from document into
        // device space by exactly the animation's offset. The loop was drawn at
        // x=80..150 on screen, so the mask lands at 40..110 in the cel — proof
        // the hit test followed the DISPLAYED ink and nothing was baked.
        INK_CHECK(std::abs(doc.selection().bounds().x - 40) < 2.0);
        // And the ants report where it DISPLAYS (device + offset).
        INK_CHECK(std::abs(doc.selection().docBounds().x - 80) < 2.0);
        std::printf("lasso over keyframed ink OK\n");
    }

    // 6. The keyframe right-click menu no longer offers a separate transform
    //    surface: the toolbar transform is the only one. Read the real menu the
    //    dock builds for a keyed frame, so this fails if the entry comes back.
    {
        Document doc(W, H, 12);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().keyCount() > 0);

        TimelineDock dock(&doc);
        QMenu *m = dock.buildKeyMenu(QString::fromStdString(a->stableId()), 0);
        INK_CHECK(m != nullptr);
        for (const QAction *act : m->actions()) {
            const QString text = act->text();
            INK_CHECK(!text.contains(QStringLiteral("Transform properties")));
            INK_CHECK(!text.contains(QStringLiteral("Properties")));
        }
        // The easing menu is still fully there — only the second transform UI
        // was removed.
        QStringList labels;
        for (const QAction *act : m->actions())
            if (!act->isSeparator() && act->isEnabled()) labels << act->text();
        INK_CHECK(labels.contains(QStringLiteral("Ease In/Out")));
        INK_CHECK(labels.contains(QStringLiteral("Open easing graph")));
        INK_CHECK(labels.contains(QStringLiteral("Delete keyframe")));
        delete m;
        std::printf("no separate transform-properties menu OK\n");
    }

    // 7. Free-mode unification (the "record changes everything" complaints):
    //    one Free mode whose bake and record paths agree on the frame, the
    //    pivot and the result; a cage that survives a release; a selection
    //    that follows a recorded edit the way it follows a baked one.
    //
    // 7a. Perspective record is authored against the CONTENT frame, exactly
    //     like the bake path — not against the canvas. Grab the content
    //     corner, drag it, and the keyed quad must take the content corner to
    //     the pointer (evaluated against the content rect).
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H); // ink at 50..150
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);

        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        Vec2 c[4];
        tool.frameCornersForTest(c); // the content corner, wherever it is
        tool.beginPrimary(ev(c[0].x, c[0].y));
        tool.continuePrimary(ev(c[0].x - 20, c[0].y - 20));
        tool.endPrimary(ev(c[0].x - 20, c[0].y - 20));
        tool.commit(); // explicit Apply (a no-op while release auto-commits)
        INK_CHECK(a->params().hasKey(ParamId::Corner0X, 0));
        // The content TL (50,50) must land 20px up-left of itself.
        const Mat3 m = a->params().matrixFor(Rect{50, 50, 100, 100}, 0.0);
        const Vec2 got = m.map({50.0, 50.0});
        INK_CHECK(std::hypot(got.x - 30.0, got.y - 30.0) < 2.0);
        std::printf("perspective record uses the content frame OK\n");
    }

    // 7b. A record session at a displaced frame draws its cage ON the
    //     displayed stroke: the recorded pose is already inside the tool
    //     state, so adding the animation offset again double-shifts it.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H); // ink at 50..150
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);

        TransformTool first(TransformTool::Mode::Free, QStringLiteral("transform"));
        first.setRecordKeyframes(true);
        first.activate(ctx);
        first.beginPrimary(ev(70, 70));
        first.continuePrimary(ev(110, 70));
        first.endPrimary(ev(110, 70));
        first.commit();
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 40.0) < 1.5);

        // A fresh session at the same frame: the stroke displays at 90..190.
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        Vec2 c[4];
        tool.frameCornersForTest(c);
        const Vec2 centre{(c[0].x + c[2].x) * 0.5, (c[0].y + c[2].y) * 0.5};
        INK_CHECK(std::hypot(centre.x - 140.0, centre.y - 100.0) < 2.0);
        // ...and the pivot sits on the displayed ink, not the stored pixels.
        const Vec2 piv = tool.pivotForTest();
        INK_CHECK(std::hypot(piv.x - 140.0, piv.y - 100.0) < 2.0);
        std::printf("record cage sits on the displayed stroke OK\n");
    }

    // 7c. The cage survives a release: ending a drag previews, and Apply
    //     lands it AND re-seeds the frame around the new state.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H); // ink at 50..150
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx); // RECORD off: bake path
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(110, 80));
        tool.endPrimary(ev(110, 80));
        INK_CHECK(tool.commit()); // the release only previewed; Apply lands it
        const std::vector<uint8_t> after = snapshot(*layer->deviceAt(0, false), W, H);
        PaintDevice probe;
        probe.fillRect(Rect{80, 50, 100, 100}, PixelRGBA8{40, 90, 200, 255}, CompositeOp::Normal);
        INK_CHECK(diffFraction(after, snapshot(probe, W, H)) < 0.05);
        // The frame was re-seeded around the moved ink (80..180).
        Vec2 c[4];
        tool.frameCornersForTest(c);
        const Vec2 centre{(c[0].x + c[2].x) * 0.5, (c[0].y + c[2].y) * 0.5};
        INK_CHECK(std::hypot(centre.x - 130.0, centre.y - 100.0) < 2.5);
        std::printf("cage persists across release + Apply OK\n");
    }

    // 7d. A recorded transform carries the SELECTION with it on screen: the
    //     cel pixels never move in record mode, so the mask stays put and the
    //     ANTS are pushed through the recorded matrix. A translation-only
    //     offset could not follow a rotation — that is the "lasso sits on its
    //     own track" complaint.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H); // ink at 50..150
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        // An off-centre patch, so a quarter turn moves its bounds visibly.
        ctx.selection().applyPolygon({{60, 60}, {100, 60}, {100, 80}, {60, 80}},
                                     SelectionAction::Replace);
        ctx.selection().setTargetId(layer->id());
        INK_CHECK(!ctx.selection().isEmpty());
        const Rect rawBefore = ctx.selection().bounds();
        INK_CHECK(rawBefore.x == 60 && rawBefore.y == 60);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        // Rotate a quarter turn about the content pivot (100,100): drag from
        // due-east of it to due-south.
        tool.beginPrimary(ev(220, 100));
        tool.continuePrimary(ev(100, 220));
        tool.endPrimary(ev(100, 220));
        tool.commit();
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Rotation, 0.0) - 90.0) < 1.0);
        // The mask is a PIXEL selection: recording never bakes, so it is
        // untouched and still selects the same stored pixels.
        INK_CHECK(ctx.selection().bounds().x == rawBefore.x);
        // The ants, however, follow the ink: (60,60)-(100,80) turned 90° about
        // (100,100) lands at (120,60)-(140,100).
        const Rect shown = ctx.selection().docBounds();
        INK_CHECK(std::abs(shown.x - 120) <= 2 && std::abs(shown.y - 60) <= 2);
        INK_CHECK(std::abs(shown.w - 20) <= 2 && std::abs(shown.h - 40) <= 2);
        std::printf("record carries the selection on screen OK\n");
    }

    // 7e. The same for a recorded TRANSLATION, and the mask must stay in
    //     device space (double-moving it would drift the pixel selection).
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H); // ink at 50..150
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        ctx.selection().applyPolygon({{60, 60}, {140, 60}, {140, 140}, {60, 140}},
                                     SelectionAction::Replace);
        ctx.selection().setTargetId(layer->id());

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(120, 80));
        tool.endPrimary(ev(120, 80));
        tool.commit();
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().isAnimated(ParamId::PositionX));
        INK_CHECK(ctx.selection().bounds().x == 60);         // device: unmoved
        INK_CHECK(ctx.selection().docBounds().x == 100);     // display: +40
        std::printf("recorded move carries the selection on screen OK\n");
    }

    // 7f. Lock-in: the BAKED path really does move the cel pixels, so there
    //     the mask itself travels (Krita's transformPixelSelectionOutline).
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        ctx.selection().applyPolygon({{60, 60}, {140, 60}, {140, 140}, {60, 140}},
                                     SelectionAction::Replace);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx); // RECORD off: bake
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(120, 80));
        tool.endPrimary(ev(120, 80));
        tool.commit();
        INK_CHECK(std::abs(ctx.selection().bounds().x - 100) <= 2);
        INK_CHECK(std::abs(ctx.selection().bounds().y - 60) <= 2);
        std::printf("bake carries the selection mask OK\n");
    }

    // 7g. Lock-in: the same Free drag bakes and records (near-)identical
    //     pixels — the two paths are one mode, not two implementations.
    {
        Document docA(W, H, 12), docB(W, H, 12);
        ToolContext ctxA(&docA), ctxB(&docB);
        NullHost hostA, hostB;
        ctxA.setHost(&hostA);
        ctxB.setHost(&hostB);
        PaintLayer *layerA = paintSquare(docA, W, H);
        PaintLayer *layerB = paintSquare(docB, W, H);
        docA.setCurrentLayer(layerA);
        docB.setCurrentLayer(layerB);
        docA.setCurrentFrame(0);
        docB.setCurrentFrame(0);
        docA.setAutoKeyEnabled(true);
        docB.setAutoKeyEnabled(true);
        docA.keyCurrentLayer(0);
        docB.keyCurrentLayer(0);

        TransformTool bake(TransformTool::Mode::Free, QStringLiteral("transform"));
        bake.activate(ctxA);
        bake.beginPrimary(ev(70, 100));
        bake.continuePrimary(ev(100, 115));
        bake.endPrimary(ev(100, 115));
        bake.commit();

        TransformTool rec(TransformTool::Mode::Free, QStringLiteral("transform"));
        rec.setRecordKeyframes(true);
        rec.activate(ctxB);
        rec.beginPrimary(ev(70, 100));
        rec.continuePrimary(ev(100, 115));
        rec.endPrimary(ev(100, 115));
        rec.commit();

        PaintDevice fa, fb;
        docA.image().project(0, fa);
        docB.image().project(0, fb);
        INK_CHECK(fa.extent().x == fb.extent().x && fa.extent().y == fb.extent().y &&
                  fa.extent().w == fb.extent().w && fa.extent().h == fb.extent().h);
        INK_CHECK(diffFraction(snapshot(fa, W, H), snapshot(fb, W, H)) < 0.005);
        std::printf("bake and record agree on pixels OK\n");
    }

    std::printf("test_keyframe_tool_flow PASS\n");
    return 0;
}
