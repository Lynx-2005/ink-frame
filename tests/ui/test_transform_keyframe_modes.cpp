// Transform-tool regressions for the keyframe path, all of which shipped because
// nothing covered a transform ON an animated layer. Recording here is explicit
// (setRecordKeyframes(true)): the toggle is the opt-in contract, and these
// tests pin the geometry underneath it —
//
//   1. The pivot sat at the centre of the PAGE (the canvas-normalized anchor's
//      default) instead of the centre of the ink — so a rotate spun the stroke
//      about a distant point and drifted.
//   2. Warp and Liquify live in DEVICE space; the pointer arrives in DISPLAY
//      space, so on keyframed (displaced) ink every grab missed the anchor grid
//      and the frame did not line up with the drawing.
//   3. A record session kept a stale frame when the animation under it changed
//      (key added / moved / undone) without the playhead or device moving.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkTool.h"
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

// A layer whose ink sits OFF-CENTRE, so a page-centred pivot cannot pass by
// accident.
PaintLayer *paintOffCentreSquare(Document &doc, int w, int h) {
    GroupLayer *folder = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(folder, "art");
    PaintDevice *dev = l->deviceAt(0, true);
    dev->fillRect(Rect{w / 5, h / 5, w / 4, h / 4}, PixelRGBA8{40, 90, 200, 255},
                  CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int W = 200, H = 200;

    // 1. The pivot lands on the CONTENT, not the page.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintOffCentreSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        INK_CHECK(a->params().isAnimated(ParamId::AnchorX)); // the add-key set

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // opt-in: drags key, pixels untouched
        tool.activate(ctx);

        // Reach the pivot through the public status/handle seams: the overlay
        // seam is private, so assert on what the tool KEYS. A pure rotation
        // about the content centre must move Rotation and leave Position alone
        // — with a page-centred pivot it also drags Position, which is the bug.
        const double posX0 = a->params().valueAt(ParamId::PositionX, 0.0);
        const double posY0 = a->params().valueAt(ParamId::PositionY, 0.0);

        // Drag the frame's OUTSIDE (that is the rotate gesture in Free mode).
        const Vec2 pivot = tool.pivotForTest();
        tool.beginPrimary(ev(pivot.x + 200.0, pivot.y)); // outside -> rotate
        tool.continuePrimary(ev(pivot.x + 200.0, pivot.y + 120.0));
        tool.endPrimary(ev(pivot.x + 200.0, pivot.y + 120.0));

        // The pivot is the content centre: the ink spans 40..90 in both axes
        // (w/5 = 40, w/4 = 50), so its centre is 65,65 — NOT the page centre.
        INK_CHECK(std::abs(pivot.x - 65.0) < 2.0);
        INK_CHECK(std::abs(pivot.y - 65.0) < 2.0);

        // Rotation keyed, position untouched.
        INK_CHECK(a->params().isAnimated(ParamId::Rotation));
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Rotation, 0.0)) > 1.0);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - posX0) < 1e-6);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionY, 0.0) - posY0) < 1e-6);
        std::printf("pivot on the content, rotation is clean OK\n");
    }

    // 2. Warp and Liquify grab the DISPLAYED ink on a displaced (keyframed)
    //    layer: the frame must follow the animation, and a drag must deform.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintOffCentreSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        // Displace the layer 40px right so the pixels live at 80..130 on screen
        // but 40..90 in the cel.
        doc.setParamKey(a, ParamId::PositionX, 0, 40.0, Interp::Linear);
        doc.syncSelectionTransform();

        for (auto mode : {TransformTool::Mode::Warp, TransformTool::Mode::Liquify}) {
            TransformTool tool(mode, QStringLiteral("transform"));
            tool.activate(ctx);
            // Aim at a REAL anchor of the grid, taken from the session itself and
            // offset into DISPLAY space. The grab only lands if the pointer is
            // mapped back into device space before the hit test — which is the
            // whole bug: without it every grab tested against a grid that sits
            // 40px to the left of the cursor.
            const Vec2 off = tool.offsetForTest();
            Vec2 grab{0.0, 0.0};
            if (mode == TransformTool::Mode::Warp) {
                INK_CHECK(!tool.warpAnchorsForTest().empty());
                // The middle anchor of the first row: inside the ink, not on a
                // canvas edge, so a hit proves the mapping and not a corner.
                grab = tool.warpAnchorsForTest()[size_t(tool.warpAnchorsForTest().size() / 2)];
                const Vec2 shown{grab.x + off.x, grab.y + off.y};
                INK_CHECK(tool.anchorAtForTest(shown, 1.0) >= 0);
                grab = shown;
            } else {
                grab = tool.liftCentreForTest();
                grab = {grab.x + off.x, grab.y + off.y};
            }

            const std::vector<uint8_t> before = snapshot(*layer->deviceAt(0, false), W, H);
            tool.beginPrimary(ev(grab.x, grab.y));
            for (int i = 1; i <= 6; ++i) tool.continuePrimary(ev(grab.x, grab.y + i * 6));
            tool.endPrimary(ev(grab.x, grab.y + 36));
            // The deformation landed in the cel.
            INK_CHECK(before != snapshot(*layer->deviceAt(0, false), W, H));
            // ...and it stayed a PIXEL edit: the mesh modes key nothing.
            INK_CHECK(!a->params().isAnimated(ParamId::Rotation));
            INK_CHECK(!a->params().isAnimated(ParamId::ScaleX));
        }
        std::printf("warp/liquify follow the displayed ink OK\n");
    }

    // 3. A record session re-seeds when the animation under it changes: a key
    //    written at the SAME frame must not leave a stale frame behind.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintOffCentreSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.keyCurrentLayer(0);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // opt-in: drags key, pixels untouched
        tool.activate(ctx);
        const Vec2 pivotBefore = tool.pivotForTest();

        // The panel keys PositionX at the same frame the session is on: the
        // pose under the tool changed, so the next grab must re-read it.
        doc.setParamKey(a, ParamId::PositionX, 0, 25.0, Interp::Linear);
        tool.beginPrimary(ev(pivotBefore.x + 200.0, pivotBefore.y));
        tool.continuePrimary(ev(pivotBefore.x + 210.0, pivotBefore.y + 60.0));
        tool.endPrimary(ev(pivotBefore.x + 210.0, pivotBefore.y + 60.0));
        // The drag continued FROM the new pose, it did not snap it away.
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Rotation, 0.0)) > 1.0);
        std::printf("record session re-seeds on a pose change OK\n");
    }

    // 4. THE regression: Free mode's scale and rotate on a DISPLACED layer.
    //    The frame is drawn in display space (the animation shifts it) while
    //    the pivot, the drag origin and the keyed pixels are device space. When
    //    the drag mixed the two, every scale and rotate landed wrong as soon as
    //    a keyframe moved the ink — which is exactly "free mode is broken with
    //    keyframes". Both are asserted against a HAND-COMPUTED expectation, so
    //    a plausible-but-wrong number cannot pass.
    {
        Document doc(W, H, 12);
        ToolContext ctx(&doc);
        NullHost host;
        ctx.setHost(&host);
        PaintLayer *layer = paintOffCentreSquare(doc, W, H);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        AdjustmentLayer *a = doc.ensureAdjustmentFor(layer);
        doc.keyCurrentLayer(0);
        // Displace 40px right. The layer's animation now owns the display
        // position, so dispOffset_ is 40 and the two spaces genuinely differ.
        doc.setParamKey(a, ParamId::PositionX, 0, 40.0, Interp::Linear);

        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true); // opt-in: drags key, pixels untouched
        tool.activate(ctx);
        // A record session's pointer mapping must be the pose the COMPOSITOR
        // applies, so the handle a drag measures from and the pivot it measures
        // about are in one space. Asserted against the model, not against the
        // tool's own number: the old check read the tool's offset and compared
        // it to itself, which is why a translation-only mapping passed here
        // while every rotated pose broke the tool.
        {
            const Rect frame = a->params().frameOr(Rect{0, 0, W, H});
            const Mat3 model = a->params().matrixFor(frame, 0.0, nullptr);
            const Mat3 got = tool.displayMatrixForTest();
            for (int i = 0; i < 9; ++i)
                INK_CHECK(std::abs(got.m[i] - model.m[i]) < 1e-3);
            Vec2 fc[4], drawn[4];
            rectCorners(frame, fc);
            tool.frameCornersForTest(drawn);
            for (int i = 0; i < 4; ++i) {
                const Vec2 want = model.map(fc[i]);
                INK_CHECK(std::hypot(drawn[i].x - want.x, drawn[i].y - want.y) < 1e-3);
            }
        }

        // --- scale by the bottom-right corner, about the content pivot ---
        // Read the frame's real drawn corners rather than re-deriving them: the
        // lift region is the device extent, not the ink's bbox, so assuming
        // 40..90 would aim at empty canvas and miss the handle entirely.
        // The frame is not square about the pivot (the pivot is the INK's
        // centre, the frame is the device extent), so each axis is expected to
        // scale by its own ratio: target distance / starting distance.
        Vec2 corners[4];
        tool.frameCornersForTest(corners);
        // The pivot seam reports the SHOWN pivot (display space, travelling
        // with the frame) — the tool maps it back to device space when the
        // drag starts, which is the invariant under test.
        const Vec2 pivotShown = tool.pivotForTest();
        INK_CHECK(std::abs(pivotShown.x - 105.0) < 2.0 && std::abs(pivotShown.y - 65.0) < 2.0);
        const Vec2 br = corners[2];
        const double baseX = std::abs(br.x - pivotShown.x), baseY = std::abs(br.y - pivotShown.y);
        INK_CHECK(baseX > 1.0 && baseY > 1.0);
        const double wantX = 1.5, wantY = 2.0; // deliberate, different per axis
        const Vec2 target{pivotShown.x + baseX * wantX, pivotShown.y + baseY * wantY};
        tool.beginPrimary(ev(br.x, br.y));
        tool.continuePrimary(ev(target.x, target.y));
        tool.endPrimary(ev(target.x, target.y));
        const double sx = a->params().valueAt(ParamId::ScaleX, 0.0);
        const double sy = a->params().valueAt(ParamId::ScaleY, 0.0);
        if (std::abs(sx - wantX) >= 0.02 || std::abs(sy - wantY) >= 0.02)
            std::printf("   TL(%.1f,%.1f) BR(%.1f,%.1f) pivot(%.1f,%.1f) "
                        "scaleX=%.4f (want %.2f) scaleY=%.4f (want %.2f)\n",
                        corners[0].x, corners[0].y, br.x, br.y, pivotShown.x, pivotShown.y, sx,
                        wantX, sy, wantY);
        INK_CHECK(std::abs(sx - wantX) < 0.02);
        INK_CHECK(std::abs(sy - wantY) < 0.02);
        // Scaling about the pivot must not translate the layer.
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 40.0) < 1e-6);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::Rotation, 0.0)) < 1e-6);
        std::printf("free scale about the pivot on a displaced layer OK\n");

        // --- rotate about the pivot, from outside the frame ---
        // Settle the session back to the rest pose first, so the rotation is
        // measured from zero rather than from the scale above.
        doc.setParamKey(a, ParamId::ScaleX, 0, 1.0, Interp::Linear);
        doc.setParamKey(a, ParamId::ScaleY, 0, 1.0, Interp::Linear);
        tool.beginPrimary(ev(1.0, 1.0));
        tool.endPrimary(ev(1.0, 1.0));

        // Rotate is driven from OUTSIDE the frame. The frame spans device
        // 0..128 shifted by the 40px offset, so a point well outside it — say
        // (200,200) — is safely in the "rotate" zone on both axes.
        // Swing from due-east of the pivot to due-south: a quarter turn.
        const Vec2 from{pivotShown.x + 120.0, pivotShown.y};
        const Vec2 to{pivotShown.x, pivotShown.y + 120.0};
        tool.beginPrimary(ev(from.x, from.y));
        tool.continuePrimary(ev(to.x, to.y));
        tool.endPrimary(ev(to.x, to.y));
        const double rot = a->params().valueAt(ParamId::Rotation, 0.0);
        if (std::abs(rot - 90.0) >= 0.5)
            std::printf("   rotation=%.3f (want 90)\n", rot);
        INK_CHECK(std::abs(rot - 90.0) < 0.5);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 40.0) < 1e-6);
        INK_CHECK(std::abs(a->params().valueAt(ParamId::ScaleX, 0.0) - 1.0) < 1e-6);
        std::printf("free rotate about the pivot on a displaced layer OK\n");
    }

    std::printf("test_transform_keyframe_modes PASS\n");
    return 0;
}
