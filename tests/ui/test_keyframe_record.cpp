// Keyframe recording, driven headlessly: with Auto-key armed and an adjustment
// layer linked to the working layer, a Transform or Move drag must write
// keyframes and leave the PIXELS ALONE. The regression half of the test is just
// as important: with no link, both tools must still bake exactly as before.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkMoveTool.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <cstdio>
#include <vector>

using namespace ink;

namespace {

struct NullHost : ToolHost {
    void repaint() override {}
    void rebuild() override {}
};

std::vector<uint8_t> snapshot(const PaintDevice &dev, int w, int h) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    const_cast<PaintDevice &>(dev).readRect({0, 0, w, h}, buf.data());
    return buf;
}

ToolEvent ev(double x, double y, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    ToolEvent e;
    e.docPos = {x, y};
    e.zoom = 1.0;
    e.mods = mods;
    return e;
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

double param(Document &doc, AdjustmentLayer *a, ParamId id, int frame) {
    Q_UNUSED(doc);
    return a->params().valueAt(id, double(frame));
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 160, H = 160;

    // 1. Transform in record mode writes keys and never touches the pixels.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);

        // Keyframes live on the layer: auto-key + a working layer means the
        // record target exists from the start — no linking step.
        INK_CHECK(ctx.recording());
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a != nullptr);
        INK_CHECK(a->target() == layer);
        INK_CHECK(ctx.recording());

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // opt-in, like the options-bar toggle
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);

        // Grab the frame (away from the pivot handle, which sits at the canvas
        // centre) and drag it 30px right.
        tool.beginPrimary(ev(100, 100));
        tool.continuePrimary(ev(130, 100));
        tool.endPrimary(ev(130, 100));
        tool.commit(); // explicit Apply: the release only previewed the keys

        // The pixels are untouched...
        const std::vector<uint8_t> after = snapshot(*layer->deviceAt(0, false), W, H);
        INK_CHECK(before == after);
        // ...and the animation carries the move.
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 0));
        INK_CHECK(std::abs(param(doc, a, ParamId::PositionX, 0) - 30.0) < 1.5);
        INK_CHECK(a->params().isAnimated(ParamId::ScaleX) == false);

        // The composite actually moved: it grew a bounding box to the right.
        PaintDevice frame;
        doc.image().project(0, frame);
        const Rect e = frame.extent();
        INK_CHECK(e.w > W / 2);

        // One undo step for the whole drag, and it puts the pixels back (they
        // never moved) and the keyframe away.
        INK_CHECK(doc.undo().canUndo());
        doc.undoEdit();
        INK_CHECK(!a->params().isAnimated(ParamId::PositionX));
        doc.redoEdit();
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 0));
        std::printf("transform record OK\n");
    }

    // 2. Perspective mode records the four corners instead of the scalars.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a);

        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        // The four corners open on the CONTENT rect (40,40,80,80 here — the
        // frame Add-key/record author against), so the top-left grip sits on
        // the ink, not the canvas corner.
        Vec2 c[4];
        tool.frameCornersForTest(c);
        tool.beginPrimary(ev(c[0].x, c[0].y));
        tool.continuePrimary(ev(c[0].x - 20, c[0].y - 20));
        tool.endPrimary(ev(c[0].x - 20, c[0].y - 20));
        tool.commit();
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(a->params().hasKey(ParamId::Corner0X, 0));
        // Frame-relative: the content TL (40,40) moved to (20,20) against the
        // (40,40,80,80) frame.
        INK_CHECK(std::abs(param(doc, a, ParamId::Corner0X, 0) - (-0.25)) < 0.02);
        INK_CHECK(std::abs(param(doc, a, ParamId::Corner0Y, 0) - (-0.25)) < 0.02);
        // Only the grabbed corner moved: the opposite one is untouched.
        INK_CHECK(!a->params().isAnimated(ParamId::Corner2X));
        std::printf("perspective record OK\n");
    }

    // 3. Move tool records a Position key for the whole layer.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a);
        INK_CHECK(doc.selection().isEmpty()); // no selection: a whole-layer move

        MoveTool tool;
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(70, 70));
        tool.continuePrimary(ev(95, 70));
        tool.endPrimary(ev(95, 70));
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 0));
        INK_CHECK(std::abs(param(doc, a, ParamId::PositionX, 0) - 25.0) < 0.6);

        // A click that never moves must not leave a key behind.
        doc.undoEdit();
        INK_CHECK(!a->params().isAnimated(ParamId::PositionX));
        tool.beginPrimary(ev(70, 70));
        tool.endPrimary(ev(70, 70));
        INK_CHECK(!a->params().isAnimated(ParamId::PositionX));
        std::printf("move record OK\n");
    }

    // 4. Regression: with no linked adjustment layer both tools still bake.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        // recordKeyframes_ stays OFF: without the toggle the tool bakes pixels
        // even though a record target exists.
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
        tool.beginPrimary(ev(60, 60)); // body: clear of the pivot (80,80)
        tool.continuePrimary(ev(90, 60));
        tool.endPrimary(ev(90, 60));
        tool.commit(); // explicit Apply: the release only previewed
        INK_CHECK(before != snapshot(*layer->deviceAt(0, false), W, H)); // pixels moved
        INK_CHECK(doc.undo().canUndo());
        doc.undoEdit();
        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        std::printf("bake regression OK\n");
    }

    // 5. Auto-key OFF means no recording, even with a link: the tools bake.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        (void)a;
        doc.setAutoKeyEnabled(false);
        INK_CHECK(!ctx.recording());
        std::printf("auto-key gate OK\n");
    }

    // 6. A recorded drag is ONE undo step, even with many mouse moves.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(ctx);
        const size_t depth = doc.undo().undoDepth();
        tool.beginPrimary(ev(100, 100));
        for (int i = 1; i <= 12; ++i) tool.continuePrimary(ev(100 + i * 2.0, 100));
        tool.endPrimary(ev(124, 100));
        tool.commit(); // one Apply = one undo step for the whole drag
        INK_CHECK(doc.undo().undoDepth() == depth + 1);
        INK_CHECK(std::abs(param(doc, a, ParamId::PositionX, 0) - 24.0) < 1.5);
        // Esc mid-drag leaves nothing behind.
        tool.beginPrimary(ev(100, 100));
        tool.continuePrimary(ev(140, 100));
        tool.cancelPrimary();
        INK_CHECK(doc.undo().undoDepth() == depth + 1);
        INK_CHECK(std::abs(param(doc, a, ParamId::PositionX, 0) - 24.0) < 1.5);
        std::printf("one undo per drag OK\n");
    }

    std::printf("test_keyframe_record PASS\n");
    return 0;
}
