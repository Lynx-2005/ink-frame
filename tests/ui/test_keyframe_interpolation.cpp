// What the animation actually DOES once a tool has written keys. The record
// suites prove a drag reaches the model; this one proves the picture follows:
//   * a key written at one frame must not disturb any other frame,
//   * between two keys the pose must actually interpolate (and Hold must hold),
//   * the COMPOSITE must move the ink - a key that only exists in the params is
//     not a keyframe, it is a number,
//   * scrubbing back and forth must be stable (no drift, no cache staleness),
//   * baking pixels and recording keys must agree on WHERE the ink ends up,
//   * and deleting a key mid-session must leave the tool showing the pose the
//     model now holds.
#include "keyframe_harness.h"
#include "ui/tools/InkTransformTool.h"
#include <memory>

using namespace ink;
using namespace ink::t;

namespace {

// A transform tool in record mode on a keyed layer, ready to drag.
std::unique_ptr<TransformTool> recorder(Rig &r, TransformTool::Mode mode) {
    auto t = std::make_unique<TransformTool>(mode, QStringLiteral("transform"));
    t->setRecordKeyframes(true);
    t->activate(r.ctx);
    return t;
}

// Grab INSIDE the frame and drag it by (dx,dy) in DISPLAY space. The grab point
// is deliberately off-centre: the rotation pivot sits at the middle of the frame
// and wins the hit test there, so a centre press moves the pivot (and keys the
// anchor) instead of translating - correct tool behaviour, wrong gesture for a
// test that means "move it".
void dragBody(TransformTool &tool, Rig &r, double dx, double dy, int steps = 6) {
    Vec2 c[4];
    tool.frameCornersForTest(c);
    const Vec2 centre{(c[0].x + c[2].x) * 0.5, (c[0].y + c[2].y) * 0.5};
    const Vec2 from{centre.x + (c[0].x - centre.x) * 0.45, centre.y + (c[0].y - centre.y) * 0.45};
    tool.beginPrimary(ev(from.x, from.y));
    for (int i = 1; i <= steps; ++i) {
        ToolEvent e = ev(from.x + dx * double(i) / steps, from.y + dy * double(i) / steps);
        e.timeSec = double(i) * 0.05;
        tool.continuePrimary(e);
    }
    tool.endPrimary(ev(from.x + dx, from.y + dy));
    (void)r;
}

// Where the COMPOSITE puts the ink's own centre, as a pixel. Read from the
// projection, never from the tool.
Rect compositeInkBox(Document &doc, int frame) {
    PaintDevice scratch;
    doc.image().project(frame, scratch);
    return scratch.contentBounds();
}

int allKeysFinite(const Rig &r, int frame) {
    return allFinite(r.adj->params(), frame) ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 1. A key at frame 4 must not touch frame 0, and vice versa. The record
    //    drag always writes at the PLAYHEAD, so this is the property that makes
    //    multi-frame animation possible at all.
    {
        Rig r;
        r.keyLayer();
        const double posX0 = r.val(ParamId::PositionX, 0);
        r.doc.setCurrentFrame(4);
        auto tool = recorder(r, TransformTool::Mode::Free);
        dragBody(*tool, r, 40, 0);
        INK_CHECK(allKeysFinite(r, 0) == 0 && allKeysFinite(r, 4) == 0);
        // The drag moved frame 4...
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 4) - posX0) > 5.0);
        // ...and left frame 0 exactly where it was.
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 0) - posX0) < 1e-6);
        // Two keys on one channel: 1 is a value, 0 is a curve.
        INK_CHECK(r.adj->params().hasKey(ParamId::PositionX, 0));
        INK_CHECK(r.adj->params().hasKey(ParamId::PositionX, 4));
        std::printf("a key at frame 4 leaves frame 0 alone OK\n");
    }

    // 2. Between two keys the pose interpolates; Hold does not. Both are set
    //    through the tool's own record path first, so this is the animation
    //    half of the recording contract.
    {
        Rig r;
        r.keyLayer();
        r.doc.setCurrentFrame(0);
        {
            auto tool = recorder(r, TransformTool::Mode::Free);
            dragBody(*tool, r, 0, 0); // a press with no motion keys nothing
        }
        // Lay down an explicit linear move: 0 at frame 0, 100 at frame 10.
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 0.0, Interp::Linear);
        r.doc.setParamKey(r.adj, ParamId::PositionX, 10, 100.0, Interp::Linear);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 5) - 50.0) < 1.0);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 2) - 20.0) < 1.0);
        // Hold: the engine decides a hold segment from the LEFT key alone (a
        // hold runs forward from wherever it was set - InkKeyframe.cpp), so the
        // mode goes on the key the segment LEAVES. Setting it on the right key
        // only would leave this segment interpolating, which is a real
        // asymmetry artists trip on, so both directions are pinned here.
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 0.0, Interp::Hold);
        r.doc.setParamKey(r.adj, ParamId::PositionX, 10, 100.0, Interp::Linear);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 5) - 0.0) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 10) - 100.0) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 9) - 0.0) < 1e-6);
        // ...and the hold governs ONE segment only: past key 10 the mode comes
        // from key 10 (Linear here), so frame 11 is interpolated by that
        // segment, not dragged back to the held value.
        r.doc.setParamKey(r.adj, ParamId::PositionX, 12, 140.0, Interp::Linear);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 11) - 120.0) < 1e-6);
        std::printf("linear interpolates, hold holds from the left key OK\n");
    }

    // 3. The COMPOSITE follows the keys. This is the difference between an
    //    animated layer and a layer with some numbers in it: the ink must be
    //    drawn somewhere else at frame 10 than at frame 0.
    {
        Rig r;
        r.keyLayer();
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 0.0, Interp::Linear);
        r.doc.setParamKey(r.adj, ParamId::PositionX, 10, 120.0, Interp::Linear);
        const Rect at0 = compositeInkBox(r.doc, 0);
        const Rect at10 = compositeInkBox(r.doc, 10);
        INK_CHECK(!at0.isEmpty() && !at10.isEmpty());
        // The composite moved by the keyed amount, in the keyed direction.
        INK_CHECK(std::abs((at10.x - at0.x) - 120) <= 2);
        INK_CHECK(std::abs(at10.y - at0.y) <= 2);
        std::printf("the composite actually moves the ink OK\n");
    }

    // 4. Scrubbing must be stable: reading the same frame twice gives the same
    //    picture, and a round trip does not drift. (The frame cache is the
    //    thing that can go stale here, so the composite is read through it.)
    {
        Rig r;
        r.keyLayer();
        r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 0.0, Interp::Linear);
        r.doc.setParamKey(r.adj, ParamId::PositionX, 6, 90.0, Interp::Linear);
        const std::vector<uint8_t> a0 = snapCell(r.doc, 0);
        const std::vector<uint8_t> a3 = snapCell(r.doc, 3);
        const std::vector<uint8_t> a6 = snapCell(r.doc, 6);
        for (int i = 0; i < 3; ++i) {
            INK_CHECK(snapCell(r.doc, 3) == a3);
            INK_CHECK(snapCell(r.doc, 0) == a0);
            INK_CHECK(snapCell(r.doc, 6) == a6);
        }
        // Mid frames are genuinely different pictures, not the same one thrice.
        INK_CHECK(a0 != a3 && a3 != a6);
        // And monotonic: the ink marches right as the frame grows.
        const Rect b0 = compositeInkBox(r.doc, 0), b3 = compositeInkBox(r.doc, 3),
                  b6 = compositeInkBox(r.doc, 6);
        INK_CHECK(b0.x <= b3.x && b3.x <= b6.x);
        std::printf("scrubbing is stable and monotonic OK\n");
    }

    // 5. Bake and record must agree on where the ink ends up. Transform with
    //    Record OFF at the playhead, then the same drag with Record ON, and the
    //    two frames must show the ink in the same place - one by rasterising it,
    //    the other by posing it. If these disagree, "record" is a different
    //    transform wearing the same name.
    {
        auto poseAt = [](const std::vector<uint8_t> &frame, Vec2 p, int r) {
            // The centre of mass of the alpha in `frame`, which is the ink
            // wherever it ended up.
            double sx = 0, sy = 0, n = 0;
            const Rect box = intersectRect(around(p, r), Rect{0, 0, kW, kH});
            for (int y = box.y; y < box.y + box.h; ++y)
                for (int x = box.x; x < box.x + box.w; ++x) {
                    const double a = frame[(size_t(y) * kW + x) * 4 + 3];
                    sx += double(x) * a;
                    sy += double(y) * a;
                    n += a;
                }
            return n > 0.0 ? Vec2{sx / n, sy / n} : Vec2{0, 0};
        };

        // (a) baked: pixels move, no keys.
        Rig baked;
        baked.keyLayer();
        const std::vector<uint8_t> bakedBefore = snapCel(baked.doc, baked.layer, 0);
        {
            auto tool = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                        QStringLiteral("transform"));
            tool->activate(baked.ctx);
            dragBody(*tool, baked, 45, 25);
            tool->commit();
        }
        const std::vector<uint8_t> bakedAfter = snapCel(baked.doc, baked.layer, 0);
        // The ink really moved in the cel: its old home is empty and it now
        // lives somewhere else.
        INK_CHECK(bakedBefore != bakedAfter);
        INK_CHECK(diffCount(bakedBefore, bakedAfter, kInk) > 0);
        INK_CHECK(nonempty(bakedAfter));

        // (b) recorded: keys move, the cel does not.
        Rig rec;
        rec.keyLayer();
        const std::vector<uint8_t> recBefore = snapCel(rec.doc, rec.layer, 0);
        const std::vector<double> recValuesBefore = rec.values(0);
        {
            auto tool = recorder(rec, TransformTool::Mode::Free);
            dragBody(*tool, rec, 45, 25);
            tool->commit();
        }
        // The cel is untouched, byte for byte...
        INK_CHECK(snapCel(rec.doc, rec.layer, 0) == recBefore);
        // ...while the model moved.
        INK_CHECK(rec.values(0) != recValuesBefore);
        INK_CHECK(std::abs(rec.val(ParamId::PositionX, 0) - 45.0) < 1.5);
        INK_CHECK(std::abs(rec.val(ParamId::PositionY, 0) - 25.0) < 1.5);

        // And the two frames show the ink in the SAME place, which is the whole
        // point: one rasterised it, the other posed it. Compared through the
        // alpha centroid, so a filter difference cannot fail this.
        const std::vector<uint8_t> bakedComposite = snapCell(baked.doc, 0);
        const std::vector<uint8_t> recComposite = snapCell(rec.doc, 0);
        const Vec2 bakedAt = poseAt(bakedComposite, {100, 100}, 95);
        const Vec2 recAt = poseAt(recComposite, {100, 100}, 95);
        INK_CHECK(std::hypot(bakedAt.x - recAt.x, bakedAt.y - recAt.y) < 3.0);
        std::printf("bake and record land the ink in the same place OK\n");
    }

    // 6. A key deleted under an open record session. The pose the cage was
    //    built from no longer exists, so the tool has to stop drawing it. The
    //    re-seed happens ON PRESS (that is the first moment the tool learns the
    //    animation moved), which means the press itself is resolved against the
    //    corrected frame - so the contract to pin is: the press that re-seeds
    //    leaves the cage equal to what the MODEL says, and the drag after it
    //    records from that corrected state.
    {
        Rig r;
        r.keyLayer();
        r.doc.setCurrentFrame(2);
        auto tool = recorder(r, TransformTool::Mode::Free);
        dragBody(*tool, r, 30, 0);
        tool->commit();
        INK_CHECK(r.adj->params().hasKey(ParamId::PositionX, 2));
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 2) - 30.0) < 1.0);

        // Delete it behind the tool's back, the way the timeline's stopwatch
        // toggle does.
        INK_CHECK(r.doc.removeParamKey(r.adj, ParamId::PositionX, 2));
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 2)) < 1e-6);

        // The press that discovers the change re-seeds. Measure the cage after
        // it: it must be the frame rect, because the model now holds an identity
        // pose (the stale cage was still showing the deleted 30px).
        {
            Vec2 stale[4];
            tool->frameCornersForTest(stale);
            const Vec2 centre{(stale[0].x + stale[2].x) * 0.5, (stale[0].y + stale[2].y) * 0.5};
            tool->beginPrimary(ev(centre.x, centre.y));
            tool->endPrimary(ev(centre.x, centre.y));
        }
        tool->commit(); // settle the empty record so the state is clean
        Vec2 fixed[4];
        tool->frameCornersForTest(fixed);
        Vec2 fc[4];
        rectCorners(r.frame, fc);
        for (int i = 0; i < 4; ++i) {
            INK_CHECK(std::hypot(fixed[i].x - fc[i].x, fixed[i].y - fc[i].y) < 1e-3);
            INK_CHECK(std::isfinite(fixed[i].x) && std::isfinite(fixed[i].y));
        }

        // And a drag from there records from the corrected state, not the stale
        // one: 40px down is 40px of Position Y, with the deleted X left alone.
        dragBody(*tool, r, 0, 40);
        INK_CHECK(std::abs(r.val(ParamId::PositionY, 2) - 40.0) < 1e-3);
        INK_CHECK(std::abs(r.val(ParamId::PositionX, 2)) < 1e-6);
        INK_CHECK(allKeysFinite(r, 2) == 0);
        std::printf("a deleted key re-seeds the session OK\n");
    }

    // 7. Undo of a record must restore the pose, and redo must bring it back -
    //    across a drag, at the frame it was made on.
    {
        Rig r;
        r.keyLayer();
        r.doc.setCurrentFrame(3);
        const std::vector<double> before = r.values(3);
        {
            auto tool = recorder(r, TransformTool::Mode::Free);
            dragBody(*tool, r, 55, -30);
            tool->commit();
        }
        INK_CHECK(r.values(3) != before);
        r.doc.undo().undo();
        INK_CHECK(r.values(3) == before);
        INK_CHECK(allKeysFinite(r, 3) == 0);
        r.doc.undo().redo();
        INK_CHECK(r.values(3) != before);
        INK_CHECK(allKeysFinite(r, 3) == 0);
        std::printf("record undo/redo round-trips the pose OK\n");
    }

    std::printf("test_keyframe_interpolation PASS\n");
    return 0;
}
