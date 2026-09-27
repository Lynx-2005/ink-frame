// Regression tests for the keyed layer + transform-tool interplay.
//
// The adjustment layer is gone: keyframes live on the paint layer, selecting
// the "animation" view selects the target layer itself, and recording is
// OPT-IN through the tool's "Record as keyframes" toggle. These pin down:
//   1. With the toggle ON a transform drag writes keys and leaves pixels alone.
//   2. The whole-stroke lasso resolves its composite from the working layer.
//   3. With no working layer at all the tool stays inert (no crash, no phantom
//      composite).
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <cstdio>

using namespace ink;

namespace {

struct NullHost : ToolHost {
    void repaint() override {}
    void rebuild() override {}
    double nowSec() const override { return ++ticks * 0.05; }
    mutable int ticks{0};
};

std::vector<uint8_t> snapshot(const PaintDevice &dev, int w, int h) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    const_cast<PaintDevice &>(dev).readRect({0, 0, w, h}, buf.data());
    return buf;
}

ToolEvent ev(double x, double y) {
    ToolEvent e;
    e.docPos = {x, y};
    e.zoom = 1.0;
    return e;
}

PaintLayer *paintSquare(Document &doc, int w, int h) {
    GroupLayer *folder = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(folder, "art");
    PaintDevice *dev = l->deviceAt(0, true);
    dev->fillRect(Rect{w / 4, h / 4, w / 2, h / 2}, PixelRGBA8{40, 90, 200, 255},
                  CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 160, H = 160;

    // 1. Transform with record mode armed: keys are written, pixels untouched.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a); // the view selects the layer itself now
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);

        INK_CHECK(ctx.doc()->currentLayer() == layer); // the working layer is the target
        INK_CHECK(ctx.recording());

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // the options-bar toggle is the gate now
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);

        tool.beginPrimary(ev(100, 100));
        tool.continuePrimary(ev(130, 100));
        tool.endPrimary(ev(130, 100));

        INK_CHECK(before == snapshot(*layer->deviceAt(0, false), W, H));
        INK_CHECK(a->params().hasKey(ParamId::PositionX, 0));
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 30.0) < 1.5);
        std::printf("record-mode transform OK\n");
    }

    // 2. Whole-stroke lasso grabs the working layer's strokes.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintSquare(doc, W, H);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.setCurrentAdjustment(a);
        doc.setAutoKeyEnabled(false);

        const PaintDevice *comp = doc.compositeOfCurrentCell();
        INK_CHECK(comp != nullptr);              // previously null: silent plain-polygon fallback
        INK_CHECK(!comp->extent().isEmpty());    // the square is there to grab
        std::printf("lasso source OK\n");
    }

    // 3. With NO working layer the tool stays inert: nothing to record, no
    //    device to lift, no phantom composite.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        // The document boots with a default "Layer 1": drop it so there is
        // genuinely no working layer.
        if (PaintLayer *boot = doc.currentLayer()) doc.deleteTrack(boot);
        INK_CHECK(doc.currentLayer() == nullptr);
        INK_CHECK(!ctx.recording());
        INK_CHECK(doc.compositeOfCurrentCell() == nullptr);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        tool.beginPrimary(ev(80, 80));
        tool.continuePrimary(ev(110, 80));
        tool.endPrimary(ev(110, 80));
        INK_CHECK(!tool.isEditing());
        std::printf("no-layer inert OK\n");
    }

    std::printf("test_adjustment_selected PASS\n");
    return 0;
}