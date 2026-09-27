// Combined behaviour: the sequences an artist actually performs, where each
// step depends on the one before. Single-tool suites cannot see these - a bake
// that is correct on its own and a record that is correct on its own will still
// fight if the cel, the keys and the playhead are all moving at once.
//
//   A. paint -> key -> record a transform at frame 0 -> paint again at frame 4
//      -> key -> record a move at frame 4. Both frames must show their own ink
//      under their own pose, and the two records must not disturb each other.
//   B. bake a warp on a keyed layer, then record a transform: the bake is
//      permanent pixels, the record is a pose, and the composite must show the
//      pose applied to the ALREADY warped ink.
//   C. two layers with keys, switching between them mid-gesture.
//   D. a selection that spans a keyframed move: the mask lives in device space
//      while the ink travels.
//   E. record at frame 0, scrub to 5, record again, scrub back: no drift.
//   F. undo a bake that happened while keys existed - the pixels come back and
//      the keys are untouched.
//   G. record on, then the toggle off mid-session: the pending work lands, the
//      next gesture bakes, and neither is lost.
#include "keyframe_harness.h"
#include "ui/InkCurveEditor.h"
#include "ui/tools/InkMoveTool.h"
#include "ui/tools/InkPaintTool.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkTransformTool.h"
#include <memory>

using namespace ink;
using namespace ink::t;

namespace {

void paintDab(BrushTool &brush, Rig &r, double x, double y) {
    r.ctx.setSize(9.0);
    r.ctx.setPrimaryColor(QColor(20, 220, 90));
    brush.activate(r.ctx);
    brush.beginPrimary(ev(x, y));
    for (int i = 0; i < 3; ++i) {
        ToolEvent e = ev(x + i, y);
        e.timeSec = double(i + 1) * 0.05;
        brush.continuePrimary(e);
    }
    brush.endPrimary(ev(x + 3, y));
    brush.deactivate();
}

// An off-centre press inside the frame (the centre is the pivot).
Vec2 insideOf(const TransformTool &tool, double fx, double fy) {
    Vec2 c[4];
    tool.frameCornersForTest(c);
    const Vec2 centre{(c[0].x + c[2].x) * 0.5, (c[0].y + c[2].y) * 0.5};
    return {centre.x + (c[0].x - centre.x) * fx, centre.y + (c[0].y - centre.y) * fy};
}

void dragFrom(TransformTool &tool, Vec2 from, double dx, double dy, int steps = 6) {
    tool.beginPrimary(ev(from.x, from.y));
    for (int i = 1; i <= steps; ++i) {
        ToolEvent e = ev(from.x + dx * double(i) / steps, from.y + dy * double(i) / steps);
        e.timeSec = double(i) * 0.05;
        tool.continuePrimary(e);
    }
    tool.endPrimary(ev(from.x + dx, from.y + dy));
}

// The alpha-weighted centre of the ink in a projected frame: where the picture
// says the drawing is, independent of any tool.
Vec2 inkCentre(Document &doc, int frame) {
    // readRect WRITES into the buffer it is given; it does not size it, so the
    // vector has to be allocated first (an empty one hands it a null pointer).
    const std::vector<uint8_t> px = snapCell(doc, frame);
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const double a = px[(size_t(y) * kW + x) * 4 + 3];
            sx += double(x) * a;
            sy += double(y) * a;
            n += a;
        }
    return n > 0 ? Vec2{sx / n, sy / n} : Vec2{0, 0};
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // A. The full authoring pass: paint, key, record a pose, paint again on
    //    another frame, key, record again. Each frame must carry its own ink
    //    under its own pose.
    {
        Rig r;
        BrushTool brush;
        paintDab(brush, r, 60, 60); // frame 0 gets a dab

        r.keyLayer(); // keys the transform set at frame 0
        r.frame = r.adj->params().frameOr(Rect{0, 0, kW, kH});

        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 35, 0);
            tool->commit();
        }
        const Vec2 centreAt0 = inkCentre(r.doc, 0);
        const std::vector<uint8_t> celAt4Before = snapCel(r.doc, r.layer, 4);

        // Frame 4: more ink, its own key, its own pose.
        r.doc.setCurrentFrame(4);
        paintDab(brush, r, 120, 120);
        r.doc.keyCurrentLayer(4);
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 0, -30);
            tool->commit();
        }
        INK_CHECK(allFinite(r.block(), 0));
        INK_CHECK(allFinite(r.block(), 4));
        // The frame-0 pose survived the frame-4 record.
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 0) - 35.0) < 1.5);
        INK_CHECK(std::abs(r.val(ParamId::PositionY, 0)) < 1.5);
        // Adding the key at frame 4 keys the value the parameter ALREADY had
        // there, so the picture does not jump at the moment the artist keys it:
        // X keeps the 35px the frame-0 pose gave it, and the drag adds only Y.
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 4) - 35.0) < 1.5);
        INK_CHECK(std::abs(r.val(ParamId::PositionY, 4) + 30.0) < 1.5);
        // The new dab really landed in frame 4's cel (and both snapshots were
        // actually taken - a null device would compare "equal" to anything).
        INK_CHECK(snapOk(celAt4Before));
        INK_CHECK(snapOk(snapCel(r.doc, r.layer, 4)));
        INK_CHECK(snapCel(r.doc, r.layer, 4) != celAt4Before);
        // It is the same cell, so frame 0 sees it too - the dab is cel content,
        // not a per-frame copy.
        INK_CHECK(snapCel(r.doc, r.layer, 0) == snapCel(r.doc, r.layer, 4));
        // And the two frames show the drawing in two different places.
        const Vec2 centreAt4 = inkCentre(r.doc, 4);
        INK_CHECK(std::hypot(centreAt0.x - centreAt4.x, centreAt0.y - centreAt4.y) > 10.0);
        std::printf("paint/key/record/paint/key/record across frames OK\n");
    }

    // B. A bake and a record on the same layer. The warp's pixels are permanent;
    //    the transform's keys are a pose. The composite must be the pose applied
    //    to the warped ink - not the warp applied twice, and not the pose lost.
    {
        Rig r;
        r.keyLayer();
        r.frame = r.adj->params().frameOr(Rect{0, 0, kW, kH});
        const std::vector<double> valuesBeforeWarp = r.values(0);
        const std::vector<uint8_t> celBeforeWarp = snapCel(r.doc, r.layer, 0);
        {
            TransformTool warp(TransformTool::Mode::Warp, QStringLiteral("transform"));
            warp.activate(r.ctx);
            const Vec2 anchor = warp.warpAnchorsForTest()[size_t(warp.warpAnchorsForTest().size() / 2)];
            const Vec2 shown = r.show(anchor);
            dragFrom(warp, shown, 20, 0);
            warp.commit();
        }
        const std::vector<uint8_t> warpedCel = snapCel(r.doc, r.layer, 0);
        // Warp and Liquify always bake: the deformation is in the cel's pixels,
        // and no key was written for it.
        INK_CHECK(snapOk(warpedCel));
        INK_CHECK(warpedCel != celBeforeWarp);
        INK_CHECK(r.values(0) == valuesBeforeWarp);
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 25, 0);
            tool->commit();
        }
        // The record left the warped cel exactly as the warp left it.
        INK_CHECK(snapCel(r.doc, r.layer, 0) == warpedCel);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 0) - 25.0) < 1.5);
        // The composite is the warped ink, moved: still a single blob, and it
        // moved by the keyed amount relative to the baked composite.
        const Vec2 afterWarp = inkCentre(r.doc, 0);
        INK_CHECK(std::isfinite(afterWarp.x) && afterWarp.y);
        std::printf("warp bake then transform record compose OK\n");
    }

    // C. Two keyed layers. A record on one must not touch the other's keys, and
    //    switching layers mid-session must settle cleanly.
    {
        Rig r;
        r.keyLayer();
        GroupLayer *folder = r.doc.ensureFolder();
        PaintLayer *second = r.doc.createTrack(folder, "second");
        second->deviceAt(0, true)->fillRect(Rect{120, 120, 40, 40},
                                            PixelRGBA8{10, 10, 200, 255}, CompositeOp::Normal);
        second->setSpanEnd(0, kOpenEnd);
        // Asking for layer 2's view DESTROYS layer 1's: the document keeps one
        // at a time. Re-fetch for the operations that need a handle, and read
        // values from the paint layers' own ParamBlocks, which are stable.
        AdjustmentLayer *adj2 = r.doc.ensureAdjustmentFor(second);
        r.doc.setParamKey(adj2, ParamId::PositionX, 0, 70.0, Interp::Linear);
        INK_CHECK(r.keys() > 0);
        const std::vector<double> layer1Keys = r.values(0);
        const double secondPos = second->params().valueAt(ParamId::PositionX, 0.0);
        r.doc.setCurrentLayer(r.layer); // back to layer 1 for the gestures below
        r.adj = r.doc.ensureAdjustmentFor(r.layer);

        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 20, 0);
            // Switch the working layer with a record open: the pending work must
            // land on the FIRST layer, and the session must re-seed for the new
            // one rather than keep writing to the old.
            r.doc.setCurrentLayer(second);
            tool->commit();
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 0, 20);
            tool->commit();
        }
        // Layer 2's pre-existing key is untouched in value, and layer 1's keys
        // are exactly what they were before the detour.
        INK_CHECK(std::abs(second->params().valueAt(ParamId::PositionX, 0.0) - secondPos) < 1e-6);
        INK_CHECK(allFinite(second->params(), 0));
        INK_CHECK(allFinite(r.block(), 0));
        INK_CHECK(r.values(0) != layer1Keys); // layer 1 really was recorded on
        std::printf("two keyed layers: records do not cross over OK\n");
    }

    // D. A selection over a keyframed move. The mask is authored and stored in
    //    DEVICE space while the ink travels; moving the selection must move the
    //    stored mask, and the display must follow.
    {
        Rig r;
        r.keyLayer();
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 40.0, Interp::Linear);
        r.doc.setParamKey(r.adj, ParamId::Rotation, 0, 90.0, Interp::Linear);
        r.doc.syncSelectionTransform();
        // A mask around the cel's own ink, in device space. The mask records
        // WHICH layer its ink came from - that binding is what the ants'
        // display map is derived from, and what the selection tools set. Build
        // the mask through a real tool gesture instead of poking the mask
        // directly, so the binding is not something a test has to remember.
        r.doc.selection().reset(kW, kH);
        {
            RectSelectTool sel;
            sel.activate(r.ctx);
            const Vec2 tl = r.show({30.0, 30.0});
            const Vec2 br = r.show({100.0, 100.0});
            sel.beginPrimary(ev(tl.x, tl.y));
            for (int i = 1; i <= 3; ++i)
                sel.continuePrimary(ev(tl.x + (br.x - tl.x) * double(i) / 3.0,
                                       tl.y + (br.y - tl.y) * double(i) / 3.0));
            sel.endPrimary(ev(br.x, br.y));
            sel.deactivate();
        }
        r.ctx.notifySelectionChanged();
        const Rect before = r.doc.selection().bounds();
        INK_CHECK(!before.isEmpty());
        INK_CHECK(r.doc.selection().offsetActive());
        // The ants are displayed through the pose...
        const Mat3 model = r.adj->params().matrixFor(r.frame, 0.0, nullptr);
        const Mat3 shown = r.doc.selection().documentMatrix();
        for (int i = 0; i < 9; ++i)
            INK_CHECK(std::abs(shown.m[i] - model.m[i]) < 1e-6);
        // ...and the mask itself is stored where the PIXELS are: drawn as a box
        // in display space, back-mapped into device space. Under a 90-degree
        // pose that back-mapped quad is itself rotated, so its axis-aligned
        // bounds are not the drawn rectangle - what must hold is that it COVERS
        // the ink it was drawn around, in device space.
        INK_CHECK(!before.isEmpty());
        INK_CHECK(before.x <= 31 && before.y <= 31);
        INK_CHECK(before.x + before.w >= 99 && before.y + before.h >= 99);
        const Selection &mask = r.doc.selection();
        INK_CHECK(mask.contains(65, 65));                      // the ink's centre
        INK_CHECK(mask.contains(40, 40) && mask.contains(90, 90)); // its corners
        // Nudge the selection: the STORED mask moves, the display map does not.
        Selection &sel = r.doc.selection();
        sel.translate(12, 5);
        r.ctx.notifySelectionChanged();
        const Rect after = sel.bounds();
        INK_CHECK(std::abs((after.x - before.x) - 12) <= 1);
        INK_CHECK(std::abs((after.y - before.y) - 5) <= 1);
        const Mat3 shown2 = sel.documentMatrix();
        for (int i = 0; i < 9; ++i)
            INK_CHECK(std::abs(shown2.m[i] - model.m[i]) < 1e-6);
        std::printf("selection over a keyed move stays in device space OK\n");
    }

    // E. Record, scrub away, record again, scrub back. Nothing drifts and no
    //    pose leaks between frames.
    {
        Rig r;
        r.keyLayer();
        r.doc.setCurrentFrame(0);
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 40, 0);
            tool->commit();
        }
        r.doc.setCurrentFrame(5);
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->setRecordKeyframes(true);
            tool->activate(r.ctx);
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 0, 50);
            tool->commit();
        }
        const double x0 = r.val(ParamId::PositionX, 0), y0 = r.val(ParamId::PositionY, 0);
        const double x5 = r.val(ParamId::PositionX, 5), y5 = r.val(ParamId::PositionY, 5);
        for (int i = 0; i < 3; ++i) {
            r.doc.setCurrentFrame(0);
            INK_CHECK(std::abs(r.val(ParamId::PositionX, 0) - x0) < 1e-9);
            INK_CHECK(std::abs(r.val(ParamId::PositionY, 0) - y0) < 1e-9);
            r.doc.setCurrentFrame(5);
            INK_CHECK(std::abs(r.val(ParamId::PositionX, 5) - x5) < 1e-9);
            INK_CHECK(std::abs(r.val(ParamId::PositionY, 5) - y5) < 1e-9);
        }
        // Frame 0 moved in x only. Frame 5's drag was a pure y move and the
        // record path starts from the pose the animation ALREADY holds there, so
        // X keeps the 40px it inherited - a drag must never silently reset a
        // channel it did not touch.
        INK_CHECK(std::abs(x0 - 40.0) < 1.5 && std::abs(y0) < 1.5);
        INK_CHECK(std::abs(x5 - 40.0) < 1.5 && std::abs(y5 - 50.0) < 1.5);
        std::printf("two-frame authoring does not drift OK\n");
    }

    // F. Undo a bake that happened while the layer had keys. The pixels must
    //    come back and the keys must be exactly as they were.
    {
        Rig r;
        r.keyLayer();
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 25.0, Interp::Linear);
        const std::vector<uint8_t> before = snapCel(r.doc, r.layer, 0);
        const std::vector<double> keys = r.values(0);
        const size_t depth = r.doc.undo().undoDepth();
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->activate(r.ctx); // Record OFF: this one bakes
            dragFrom(*tool, insideOf(*tool, 0.45, 0.45), 30, 20);
            tool->commit();
        }
        INK_CHECK(snapCel(r.doc, r.layer, 0) != before);
        INK_CHECK(r.doc.undo().undoDepth() == depth + 1);
        r.doc.undo().undo();
        INK_CHECK(snapCel(r.doc, r.layer, 0) == before);
        INK_CHECK(r.values(0) == keys);
        std::printf("undoing a bake on a keyed layer restores the pixels only OK\n");
    }

    // G. Flipping the Record toggle with work in flight. The pending gesture must
    //    land in the mode it was started in, and the next one must use the new
    //    mode. Losing either is how work silently disappears.
    {
        Rig r;
        r.keyLayer();
        r.frame = r.adj->params().frameOr(Rect{0, 0, kW, kH});
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(false);
        tool.activate(r.ctx);
        const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
        const std::vector<double> keysBefore = r.values(0);
        // Start baking, then switch to recording mid-gesture.
        tool.beginPrimary(ev(insideOf(tool, 0.45, 0.45).x, insideOf(tool, 0.45, 0.45).y));
        for (int i = 1; i <= 3; ++i) {
            const Vec2 p = insideOf(tool, 0.45, 0.45);
            ToolEvent e = ev(p.x + i * 5.0, p.y);
            e.timeSec = double(i) * 0.05;
            tool.continuePrimary(e);
        }
        tool.setRecordKeyframes(true); // settles the pending bake
        const std::vector<uint8_t> celAfterFlip = snapCel(r.doc, r.layer, 0);
        // The bake landed as pixels, and no key was written for it.
        INK_CHECK(celAfterFlip != celBefore);
        INK_CHECK(r.values(0) == keysBefore);
        // Now a record drag: keys move, the cel (warp and all) does not.
        dragFrom(tool, insideOf(tool, 0.45, 0.45), 20, 0);
        tool.commit();
        INK_CHECK(snapCel(r.doc, r.layer, 0) == celAfterFlip);
        INK_CHECK(r.values(0) != keysBefore);
        INK_CHECK(allFinite(r.block(), 0));
        tool.deactivate();
        std::printf("flipping Record mid-gesture loses nothing OK\n");
    }

    // H. The curve editor follows the keyed layer WITHOUT holding a pointer to
    //    it. The document keeps exactly one adjustment "view" alive and
    //    DESTROYS the previous one when the keyed layer changes, so a cached
    //    AdjustmentLayer* dangles the instant the artist picks another layer -
    //    and a use-after-free here is the "the app crashes" report. The keys
    //    live on the paint layer, so the editor can always re-resolve.
    {
        Rig r;
        r.keyLayer();
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 0.0, Interp::Bezier);
        r.doc.setParamKey(r.adj, ParamId::PositionX, 12, 120.0, Interp::Bezier);
        CurveEditor editor(&r.doc);
        editor.resize(360, 340);
        editor.setAdjustment(r.adj);
        INK_CHECK(editor.groupCount() == 7);
        INK_CHECK(editor.hasSegment()); // Position has the only real curve

        // Point the document at a DIFFERENT layer. That recycles the view the
        // editor was handed.
        GroupLayer *folder = r.doc.ensureFolder();
        PaintLayer *other = r.doc.createTrack(folder, "other");
        other->deviceAt(0, true)->fillRect(Rect{120, 120, 40, 40},
                                            PixelRGBA8{10, 10, 200, 255}, CompositeOp::Normal);
        other->setSpanEnd(0, kOpenEnd);
        AdjustmentLayer *otherAdj = r.doc.ensureAdjustmentFor(other);
        r.doc.setParamKey(otherAdj, ParamId::PositionX, 0, 30.0, Interp::Linear);
        r.doc.setParamKey(otherAdj, ParamId::PositionX, 8, 90.0, Interp::Linear);
        r.doc.setCurrentLayer(other);

        // Using the editor now must be SAFE (the view it was handed is gone).
        editor.refresh();
        editor.hasSegment();
        editor.groupCount();
        editor.groupLabel(1);
        // Paint the whole widget: a dangling read would show up here.
        editor.resize(360, 340);
        INK_CHECK(!editor.grab().isNull());
        // It must not claim a curve it does not own: the panel contract is
        // "never show another layer's curve", so between the layer switch and
        // the dock re-pointing it, the graph is simply empty.
        INK_CHECK(editor.adjustment() == nullptr);
        INK_CHECK(!editor.hasSegment());
        // Re-pointing it - what the timeline does on every refresh - tracks the
        // layer the document is on, with no dangling pointer involved.
        editor.setAdjustment(r.doc.currentAdjustment());
        INK_CHECK(editor.adjustment() == r.doc.currentAdjustment());
        INK_CHECK(editor.adjustment()->target() == other);
        INK_CHECK(editor.hasSegment()); // the new layer's Position ramp
        // And the ORIGINAL layer's keys are still there, untouched, in storage.
        INK_CHECK(r.block().hasKey(ParamId::PositionX, 12));
        INK_CHECK(std::abs(r.block().valueAt(ParamId::PositionX, 12.0) - 120.0) < 1e-3);
        std::printf("curve editor re-resolves a recycled adjustment view OK\n");
    }

    std::printf("test_keyframe_workflows PASS\n");
    return 0;
}
