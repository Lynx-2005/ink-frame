// Transform tool session loop, driven headlessly: lift -> drag -> apply -> undo,
// plus the revert path for each mode. This is the part of the feature that unit
// tests cannot reach (it needs a Document, a ToolContext and Qt cursors).
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <cstdio>
#include <vector>

using namespace ink;

namespace {

// The tool talks to the canvas through ToolHost; the test only needs the tool
// to be able to ask for a redraw.
struct NullHost : ToolHost {
    void repaint() override {}
    void rebuild() override {}
};

std::vector<uint8_t> snapshot(const PaintDevice &dev, int w, int h) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    const_cast<PaintDevice &>(dev).readRect({0, 0, w, h}, buf.data());
    return buf;
}

bool same(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) { return a == b; }

ToolEvent ev(double x, double y, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    ToolEvent e;
    e.docPos = {x, y};
    e.zoom = 1.0;
    e.mods = mods;
    return e;
}

PaintLayer *makeLayer(Document &doc, const char *name) {
    GroupLayer *folder = doc.ensureFolder();
    return doc.createTrack(folder, name);
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const int W = 200, H = 200;
    Document doc(W, H, 12);
    ToolContext ctx(&doc);
    NullHost host;
    ctx.setHost(&host);

    PaintLayer *layer = makeLayer(doc, "art");
    INK_CHECK(layer != nullptr);
    doc.setCurrentLayer(layer);
    doc.setCurrentFrame(0);

    PaintDevice *dev = ctx.editableDevice();
    INK_CHECK(dev != nullptr);
    dev->fillRect({50, 50, 40, 30}, {255, 0, 0, 255}); // the "drawing"
    doc.touchLayer(layer, 0);
    const std::vector<uint8_t> original = snapshot(*dev, W, H);

    // --- free transform: drag the body by (+50, +50); Apply lands it ---
    {
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        INK_CHECK(!tool.isEditing()); // nothing pending until a drag happens

        // Inside the content (50..90 x 50..80) yet clear of the pivot (70,65)
        // and of every handle: the lift is pixel-precise now, so 50,55 sits ON
        // the left edge and would grab a scale, not a move.
        ToolEvent e = ev(61, 61);
        tool.beginPrimary(e);
        e = ev(111, 111); // drag by (+50, +50)
        tool.continuePrimary(e);
        tool.endPrimary(e);

        // Explicit Apply (Krita model): the release only previews.
        INK_CHECK(tool.isEditing());
        INK_CHECK(dev->pixel(105, 105).r > 200); // preview already shows it
        tool.commit();
        INK_CHECK(!tool.isEditing());
        INK_CHECK(dev->pixel(105, 105).r > 200);   // content arrived
        INK_CHECK(dev->pixel(105, 105).a > 200);
        INK_CHECK(dev->pixel(55, 55).a == 0);      // and left the old place

        INK_CHECK(doc.undo().canUndo());
        doc.undo().undo();
        INK_CHECK(same(snapshot(*dev, W, H), original)); // one undo step, exact
        doc.undo().redo();
        INK_CHECK(dev->pixel(105, 105).r > 200);
        std::printf("free transform session OK\n");
    }

    // --- free transform: Esc mid-drag puts the pixels back ---
    {
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        ToolEvent e = ev(105, 105);
        tool.beginPrimary(e);
        e = ev(70, 40);
        tool.continuePrimary(e);
        INK_CHECK(tool.isEditing());
        INK_CHECK(!same(snapshot(*dev, W, H), before));
        tool.cancelPrimary(); // reverts before release commits anything
        INK_CHECK(!tool.isEditing());
        INK_CHECK(same(snapshot(*dev, W, H), before));
        std::printf("free transform revert OK\n");
    }

    // --- rotate 90° through the options path stays lossless ---
    {
        // The lift is the PIXEL-PRECISE ink bounds now, so a flip mirrors about
        // the content's own centre — and a symmetric solid flipped about its
        // own centre is a visual no-op. Mark one corner so the flip is
        // observable; the contract under test is that it lands as one lossless
        // undo step.
        dev->fillRect({100, 100, 10, 30}, {0, 255, 0, 255});
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        tool.flipSelection(true); // one-shot: mirrors the frame and lands at once
        INK_CHECK(!tool.isEditing());
        INK_CHECK(!same(snapshot(*dev, W, H), before));
        doc.undo().undo();
        INK_CHECK(same(snapshot(*dev, W, H), before));
        std::printf("flip commit OK\n");
    }

    // --- warp: dragging an anchor deforms, Esc mid-drag restores ---
    {
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        TransformTool tool(TransformTool::Mode::Warp, QStringLiteral("warp"));
        tool.activate(ctx);
        // Density 3 spreads anchors over the PIXEL-precise ink bounds
        // (100,100)-(140,130): grab the (1,1) anchor, well inside the drawing.
        ToolEvent e = ev(113.3, 110.0);
        tool.beginPrimary(e);
        e = ev(126.0, 120.0);
        tool.continuePrimary(e);
        INK_CHECK(tool.isEditing());
        INK_CHECK(!same(snapshot(*dev, W, H), before));
        tool.cancelPrimary();
        INK_CHECK(same(snapshot(*dev, W, H), before));
        std::printf("warp session OK\n");
    }

    // --- liquify: a brush stroke deforms, Esc mid-stroke restores ---
    // (The stroke stays inside the ink at 100..140 x 100..130, which is where
    // the first block's applied +50,+50 move left it.)
    {
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        TransformTool tool(TransformTool::Mode::Liquify, QStringLiteral("liquify"));
        tool.activate(ctx);
        ToolEvent e = ev(110, 110);
        tool.beginPrimary(e);
        for (int i = 1; i <= 8; ++i) {
            ToolEvent m = ev(110 + i * 3.0, 110.0);
            m.timeSec = double(i) * 0.04; // past the preview throttle
            tool.continuePrimary(m);
        }
        INK_CHECK(tool.isEditing());
        INK_CHECK(!same(snapshot(*dev, W, H), before));
        tool.cancelPrimary();
        INK_CHECK(same(snapshot(*dev, W, H), before));
        std::printf("liquify session OK\n");
    }

    // --- with a selection the lift is the selection, and it follows the pixels ---
    {
        doc.selection().reset(W, H);
        doc.selection().applyPolygon({{120, 120}, {160, 120}, {160, 150}, {120, 150}},
                                     SelectionAction::Replace, false);
        ctx.notifySelectionChanged();
        const Rect selBefore = doc.selection().bounds();
        INK_CHECK(!selBefore.isEmpty());

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.activate(ctx);
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        // Clear of the pivot AND of the shear band: the whole edge of the frame
        // shears within the grab radius, exactly like Krita's free transform.
        ToolEvent e = ev(131, 130);
        tool.beginPrimary(e);
        e = ev(151, 140); // drag by (+20, +10)
        tool.continuePrimary(e);
        tool.endPrimary(e);
        tool.commit(); // explicit Apply: the release only previewed
        INK_CHECK(!tool.isEditing()); // landed; selection travels too
        INK_CHECK(dev->pixel(125, 125).r > 200); // moved, still clipped to the selection
        const Rect selAfter = doc.selection().bounds();
        INK_CHECK(selAfter.x == selBefore.x + 20 && selAfter.y == selBefore.y + 10);
        INK_CHECK(selAfter.w == selBefore.w && selAfter.h == selBefore.h);
        doc.undo().undo();
        INK_CHECK(same(snapshot(*dev, W, H), before));
        doc.selection().clear();
        ctx.notifySelectionChanged();
        std::printf("selection-scoped transform OK\n");
    }

    // --- one-shot menu ops: rotate 90° CCW about the region centre ---
    {
        const std::vector<uint8_t> before = snapshot(*dev, W, H);
        INK_CHECK(applyRegionOp(ctx, RegionOp::RotateCCW));
        INK_CHECK(!same(snapshot(*dev, W, H), before));
        doc.undo().undo();
        INK_CHECK(same(snapshot(*dev, W, H), before));
        std::printf("region op OK\n");
    }

    std::printf("test_transform_tool PASS\n");
    return 0;
}
