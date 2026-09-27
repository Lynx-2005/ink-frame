// The selection FOLLOWS keyframed ink: with a Position key on the linked
// adjustment layer, the marching-ants outline reports the stroke's DISPLAYED
// position as the playhead moves, a new whole-stroke lasso drawn at the moved
// position still grabs it, and the transform tool's frame is seeded where the
// stroke displays. Nothing here may bake the pose into the mask: moving the
// playhead back must move the ants back too.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <cstdio>

using namespace ink;

namespace {

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
    dev->fillRect(Rect{60, 60, 40, 40}, PixelRGBA8{40, 90, 200, 255}, CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 200, H = 200;

    Document doc(W, H, 12);
    ToolContext ctx(&doc);
    struct NullHost : ToolHost {
        void repaint() override {}
        void rebuild() override {}
    } host;
    ctx.setHost(&host);
    PaintLayer *layer = paintSquare(doc, W, H);
    doc.setCurrentLayer(layer);
    doc.setAutoKeyEnabled(true);

    // Lasso a stroke at frame 0 (no animation yet).
    LassoSelectTool lasso;
    lasso.activate(ctx);
    lasso.beginPrimary(ev(50, 50));
    for (int i = 1; i <= 8; ++i)
        lasso.continuePrimary(ev(50 + i * 5, 50));
    for (int i = 0; i <= 4; ++i)
        lasso.continuePrimary(ev(90, 90 + i * 5));
    lasso.endPrimary(ev(90, 110));
    INK_CHECK(!doc.selection().isEmpty());
    INK_CHECK(doc.selection().targetId() == layer->id());
    const Rect restBounds = doc.selection().bounds();
    INK_CHECK(restBounds.w > 20 && restBounds.h > 10);

    // Keyframe the whole layer 40px right at frame 10 (position key), rest at 0.
    AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
    doc.setParamKey(a, ParamId::PositionX, 0, 0.0, Interp::Linear);
    doc.setParamKey(a, ParamId::PositionX, 10, 40.0, Interp::Linear);

    // Frame 0: nothing moved yet.
    doc.setCurrentFrame(0);
    doc.syncSelectionTransform();
    INK_CHECK(!doc.selection().offsetActive());
    INK_CHECK(doc.selection().outline().empty() == false);
    const float xAt0 = doc.selection().outline().front()[0];

    // Frame 10: the animation holds the stroke 40px right — the ants MUST sit
    // on the moved stroke (this is the bug: they stayed at frame 0's position).
    doc.setCurrentFrame(10);
    doc.syncSelectionTransform();
    INK_CHECK(doc.selection().offsetActive());
    INK_CHECK(std::abs(doc.selection().documentOffset().x - 40.0) < 0.5);
    const float xAt10 = doc.selection().outline().front()[0];
    INK_CHECK(std::abs((xAt10 - xAt0) - 40.0) < 1.5); // outline shifted by the key

    // The raw mask did NOT move (nothing baked): same bounds, same offset back.
    INK_CHECK(doc.selection().bounds().x == restBounds.x);
    doc.setCurrentFrame(0);
    doc.syncSelectionTransform();
    INK_CHECK(!doc.selection().offsetActive());

    // A lasso drawn at the MOVED position (frame 10) still grabs the stroke:
    // applyPolygon maps the document polygon back into device space.
    doc.setCurrentFrame(10);
    doc.syncSelectionTransform();
    Selection &sel = doc.selection();
    const int midX = restBounds.x + restBounds.w / 2 + 40; // displayed centre
    const int midY = restBounds.y + restBounds.h / 2;
    sel.applyPolygon({{double(midX - 30), double(midY - 20)}, {double(midX + 30), double(midY - 20)},
                      {double(midX + 30), double(midY + 20)}, {double(midX - 30), double(midY + 20)}},
                     SelectionAction::Replace, true);
    INK_CHECK(!sel.isEmpty());
    // The polygon was mapped back into device space by EXACTLY the offset:
    // drawn at 80..140 (document), stored at 40..100 (device).
    INK_CHECK(std::abs(sel.bounds().x - (midX - 30 - 40)) < 1.5);
    std::printf("ants follow keyframes OK\n");

    // The transform tool seeds its frame on the DISPLAYED stroke: with the
    // offset active, the session carries that offset, so the frame it draws and
    // the corners it hit-test both sit on the moved ink. Activating alone must
    // not move any pixels.
    TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
    tool.activate(ctx);
    INK_CHECK(std::abs(tool.offsetForTest().x - 40.0) < 0.5);
    INK_CHECK(!tool.isEditing());
    std::printf("transform session seeds display pose OK\n");

    std::printf("test_selection_follows_keys PASS\n");
    return 0;
}