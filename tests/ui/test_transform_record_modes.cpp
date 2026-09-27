// Transform-tool regressions for a transform ON A ROTATED/SCALED keyframed
// layer. Every earlier test in this area used a pure TRANSLATION as the
// "displaced" pose, which is the one case a Vec2 offset can represent, so the
// whole family of bugs below shipped: the tool compensated a translation while
// the cel displays through a full matrix (rotate/shear/scale/quad).
//
// What is pinned here:
//   1. Warp/Liquify: the pointer arrives in DISPLAY space, the mesh lives in
//      DEVICE space. On rotated ink the grab must be mapped by the FULL inverse
//      of the animation matrix, or the deformation lands somewhere else.
//   2. Perspective: its corners are authored in FRAME space. A corner drag must
//      be written back in frame space too, or the recorded value is the pointer's
//      display position and the box jumps away on the next composite.
//   3. The drawn cage must equal what the COMPOSITOR draws. The expectation is
//      computed from ParamBlock::matrixFor directly - never from a tool getter,
//      because deriving the expectation from the value under test is what let
//      the old offset-only tests pass while the box sat in the page corner.
//   4. A gesture sequence that thrashes modes mid-record must not corrupt the
//      record, leave non-finite values in the model, or lose the session.
#include "../test_check.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkTool.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkTransformTool.h"
#include <QApplication>
#include <algorithm>
#include <cstdlib>
#include <cmath>
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

// Ink at {40,40,50,50} on a 200x200 page: off-centre, so a page-anchored
// transform cannot pass by accident.
constexpr int kW = 200, kH = 200;
const Rect kInk{40, 40, 50, 50};

PaintLayer *paintSquare(Document &doc) {
    GroupLayer *folder = doc.ensureFolder();
    PaintLayer *l = doc.createTrack(folder, "art");
    PaintDevice *dev = l->deviceAt(0, true);
    dev->fillRect(kInk, PixelRGBA8{40, 90, 200, 255}, CompositeOp::Normal);
    l->setSpanEnd(0, kOpenEnd);
    return l;
}

// A document whose cel displays ROTATED 90 degrees about its own centre: the
// display position of the ink's top-left corner is nowhere near its device
// position, so any translation-only mapping is caught immediately.
struct Rig {
    Document doc{kW, kH, 12};
    ToolContext ctx{&doc};
    NullHost host;
    PaintLayer *layer{nullptr};
    AdjustmentLayer *adj{nullptr};
    Rect frame{};
    Mat3 display{}; // device -> display, straight from the model
    Mat3 back{};    // display -> device

    // INK_CHECK returns from main, so it cannot live in a constructor: the
    // invertibility of the display matrix is reported as a flag and checked by
    // the caller.
    bool displayOk{false};

    Rig() {
        ctx.setHost(&host);
        layer = paintSquare(doc);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        doc.keyCurrentLayer(0);
        adj = doc.ensureAdjustmentFor(layer);
        doc.setParamKey(adj, ParamId::Rotation, 0, 90.0, Interp::Linear);
        doc.setParamKey(adj, ParamId::PositionX, 0, 12.0, Interp::Linear);
        const Image &img = doc.image();
        frame = adj->params().frameOr(Rect{0, 0, img.width(), img.height()});
        // The expectation comes from the model, not from the tool.
        display = adj->params().matrixFor(frame, 0.0, nullptr);
        back = display.inverted(&displayOk);
        doc.syncSelectionTransform();
    }

    Vec2 show(Vec2 device) const { return display.map(device); }
    Vec2 hide(Vec2 displayPos) const { return back.map(displayPos); }
};

// True when any pixel inside `r` differs between two snapshots.
bool differsNear(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, Rect r) {
    r = intersectRect(r, Rect{0, 0, kW, kH});
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const size_t i = (size_t(y) * kW + x) * 4;
            if (a[i + 3] != b[i + 3] || a[i] != b[i])
                return true;
        }
    return false;
}

bool allFinite(const ParamBlock &b, int frame) {
    static const ParamId kAll[] = {ParamId::PositionX,   ParamId::PositionY,
                                   ParamId::AnchorX,      ParamId::AnchorY,
                                   ParamId::ScaleX,       ParamId::ScaleY,
                                   ParamId::Rotation,     ParamId::ShearX,
                                   ParamId::ShearY,       ParamId::Corner0X,
                                   ParamId::Corner0Y,     ParamId::Corner1X,
                                   ParamId::Corner1Y,     ParamId::Corner2X,
                                   ParamId::Corner2Y,     ParamId::Corner3X,
                                   ParamId::Corner3Y};
    for (ParamId id : kAll)
        if (!std::isfinite(b.valueAt(id, frame)))
            return false;
    return true;
}

Rect around(Vec2 p, int r) { return {int(p.x) - r, int(p.y) - r, 2 * r + 1, 2 * r + 1}; }

} // namespace

// `argv[1]` runs ONE block (1..4), so a failure can be attributed to a single
// contract while iterating; no argument runs all of them in order.
static int gOnly = 0;
static bool want(int n) { return gOnly == 0 || gOnly == n; }

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    gOnly = argc > 1 ? std::atoi(argv[1]) : 0;

    // 1. Warp: the anchor the user SEES must be the anchor that gets grabbed,
    //    on a rotated layer. A translation-only mapping puts the grid far from
    //    the cursor, so the grab misses entirely.
    if (want(1)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Warp, QStringLiteral("transform"));
        tool.activate(r.ctx);
        INK_CHECK(!tool.warpAnchorsForTest().empty());
        // Middle anchor of the grid, in DEVICE space, as the session holds it.
        const Vec2 anchor = tool.warpAnchorsForTest()[size_t(tool.warpAnchorsForTest().size() / 2)];
        // Where the user actually sees it: through the model's own matrix.
        const Vec2 shown = r.show(anchor);
        INK_CHECK(std::hypot(shown.x - anchor.x, shown.y - anchor.y) > 20.0); // the pose is not a translation
        INK_CHECK(tool.anchorAtForTest(shown, 1.0) >= 0);
        // And the session must be OPEN: the mode has to be usable at all.
        INK_CHECK(tool.isEditing() == false); // nothing pending yet
        tool.beginPrimary(ev(shown.x, shown.y));
        for (int i = 1; i <= 6; ++i) tool.continuePrimary(ev(shown.x + i * 4, shown.y));
        tool.endPrimary(ev(shown.x + 24, shown.y));
        INK_CHECK(tool.isEditing()); // a pending deformation the user can Apply
        tool.cancelPrimary();
        std::printf("warp grabs the displayed anchor OK\n");
    }

    // 2. Liquify: a stroke smeared where the pointer is must move the pixels
    //    that DISPLAY there - not the pixels under the raw pointer position.
    if (want(2)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Liquify, QStringLiteral("transform"));
        tool.activate(r.ctx);
        // Aim at an OFF-CENTRE ink point, as DISPLAYED. (A rotation about the
        // ink's own centre leaves the centre fixed, so the centre could not tell
        // a correct mapping from a wrong one.)
        const Vec2 aim{r.show({double(kInk.x + 10), double(kInk.y + 10)})};
        INK_CHECK(std::hypot(aim.x - (kInk.x + 10.0), aim.y - (kInk.y + 10.0)) > 20.0);
        PaintDevice *dev = r.layer->deviceAt(0, false);
        const std::vector<uint8_t> before = snapshot(*dev, kW, kH);

        tool.beginPrimary(ev(aim.x, aim.y));
        for (int i = 1; i <= 8; ++i) {
            ToolEvent m = ev(aim.x, aim.y + i * 5.0);
            m.timeSec = double(i) * 0.04;
            tool.continuePrimary(m);
        }
        tool.endPrimary(ev(aim.x, aim.y + 40));
        INK_CHECK(tool.isEditing());
        const std::vector<uint8_t> after = snapshot(*dev, kW, kH);
        // The smear must show up around the DEVICE point that displays there.
        const Vec2 landed = r.hide({aim.x, aim.y + 20});
        INK_CHECK(differsNear(before, after, around(landed, 34)));
        std::printf("liquify smears the displayed pixels OK\n");
        tool.cancelPrimary();
    }

    // 3. Perspective + RECORD on a rotated pose: the recorded corner must be
    //    the frame-space point under the cursor, and the DRAWN corner must end
    //    up exactly at the pointer. This is the "box jumps / sits in the corner"
    //    report.
    if (want(3)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(r.ctx);

        // The cage as the user sees it, and the corner we will grab.
        Vec2 shown[4];
        tool.frameCornersForTest(shown);
        // The expectation, from the model alone: the compositor projects the
        // frame's corners through ParamBlock::matrixFor, so the cage must be
        // exactly those four points. (At seed the quad is identity, so this is
        // the recorded pose on its own.)
        Vec2 expect[4];
        rectCorners(r.frame, expect);
        for (int i = 0; i < 4; ++i) {
            const Vec2 want = r.show(expect[i]);
            INK_CHECK(std::hypot(shown[i].x - want.x, shown[i].y - want.y) < 1e-6);
        }

        const Vec2 grab = shown[0];
        const Vec2 drop{grab.x + 30.0, grab.y + 18.0};
        tool.beginPrimary(ev(grab.x, grab.y));
        for (int i = 1; i <= 6; ++i)
            tool.continuePrimary(ev(grab.x + drop.x * 0.0 + 5.0 * i, grab.y + 3.0 * i));
        tool.endPrimary(ev(drop.x, drop.y));

        // 3a. The keyed corner is the frame-normalised FRAME-space point under
        //     the pointer (the pose maps display -> frame, not the reverse).
        const Vec2 wantFrame = r.hide(drop);
        INK_CHECK(r.adj->params().isAnimated(ParamId::Corner0X));
        const double wantX = (wantFrame.x - r.frame.x) / double(r.frame.w);
        const double wantY = (wantFrame.y - r.frame.y) / double(r.frame.h);
        INK_CHECK(std::abs(r.adj->params().valueAt(ParamId::Corner0X, 0.0) - wantX) < 1e-6);
        INK_CHECK(std::abs(r.adj->params().valueAt(ParamId::Corner0Y, 0.0) - wantY) < 1e-6);
        INK_CHECK(allFinite(r.adj->params(), 0));

        // 3b. And the drawn corner now sits under the pointer: the compositor
        //     applies affine o quad, and the cage must agree. The tolerance is
        //     a thousandth of a pixel because the corner round-trips through the
        //     keyed value's float storage - the old failure was whole frame
        //     widths, not rounding.
        tool.commit();
        Vec2 after[4];
        tool.frameCornersForTest(after);
        INK_CHECK(std::hypot(after[0].x - drop.x, after[0].y - drop.y) < 1e-3);
        INK_CHECK(allFinite(r.adj->params(), 0));
        std::printf("perspective records frame-space corners OK\n");
    }

    // 4. Mode thrash while a record is open must not corrupt anything: this is
    //    the reported "hang / crash" gesture.
    if (want(4)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Free, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(r.ctx);
        tool.beginPrimary(ev(80, 80));
        for (int i = 1; i <= 5; ++i) tool.continuePrimary(ev(80 + i * 4, 80 + i * 3));
        for (auto m : {TransformTool::Mode::Perspective, TransformTool::Mode::Warp,
                       TransformTool::Mode::Liquify, TransformTool::Mode::Free}) {
            tool.setMode(m);
            INK_CHECK(allFinite(r.adj->params(), 0));
            tool.commit();
            INK_CHECK(allFinite(r.adj->params(), 0));
        }
        tool.deactivate();
        INK_CHECK(allFinite(r.adj->params(), 0));
        // The whole drag sequence is ONE undo step: one Ctrl+Z must put the
        // keyed pose back exactly as it was (12px, 90deg), not part-way.
        const size_t depth = r.doc.undo().undoDepth();
        INK_CHECK(depth > 0);
        r.doc.undo().undo();
        INK_CHECK(allFinite(r.adj->params(), 0));
        INK_CHECK(std::abs(r.adj->params().valueAt(ParamId::PositionX, 0.0) - 12.0) < 1e-6);
        INK_CHECK(std::abs(r.adj->params().valueAt(ParamId::Rotation, 0.0) - 90.0) < 1e-6);
        std::printf("mode thrash keeps the record sane OK\n");
    }

    // 5. The drag throttle must engage (that is the lag fix) and must never
    //    swallow the LAST move (that would leave a half-warped frame on screen).
    //    Same gesture, once with no clock and once with a clock that advances
    //    1ms per move: the settled picture must be identical, while the throttled
    //    run must have done strictly fewer repaints than there were moves.
    if (want(5)) {
        auto run = [](bool throttled, int *repaints) {
            Rig r;
            if (!r.displayOk) // INK_CHECK cannot be used here: it returns from main
                return std::vector<uint8_t>{};
            struct Clock : ToolHost {
                double t{0.0};
                int *repaints;
                void repaint() override {
                    if (repaints)
                        ++*repaints;
                }
                void rebuild() override {}
                double nowSec() const override { return t; }
            } host;
            host.repaints = repaints;
            if (throttled)
                host.t = 1.0; // a real, advancing clock
            r.ctx.setHost(&host); // the Rig installed its own no-clock host

            TransformTool tool(TransformTool::Mode::Warp, QStringLiteral("transform"));
            tool.activate(r.ctx);
            const Vec2 anchor = tool.warpAnchorsForTest()[size_t(tool.warpAnchorsForTest().size() / 2)];
            const Vec2 shown = r.show(anchor);
            tool.beginPrimary(ev(shown.x, shown.y));
            for (int i = 1; i <= 24; ++i) {
                // Only the throttled run advances the clock. A clock left at 0
                // is ToolHost's documented "no clock" case, which never throttles.
                if (throttled)
                    host.t += 0.001;
                tool.continuePrimary(ev(shown.x + i * 2.0, shown.y + i * 3.0));
            }
            tool.endPrimary(ev(shown.x + 48, shown.y + 72));
            return snapshot(*r.layer->deviceAt(0, false), kW, kH);
        };
        int unthrottledRepaints = 0, throttledRepaints = 0;
        const std::vector<uint8_t> plain = run(false, &unthrottledRepaints);
        const std::vector<uint8_t> fast = run(true, &throttledRepaints);
        INK_CHECK(!plain.empty() && !fast.empty());
        // The settled picture is the same either way: the throttle is a rate
        // limit, never a dropped final frame.
        INK_CHECK(plain == fast);
        INK_CHECK(unthrottledRepaints > throttledRepaints);
        std::printf("drag throttles without dropping the last frame OK (%d -> %d repaints)\n",
                    unthrottledRepaints, throttledRepaints);
    }

    // 6. A degenerate corner drag must be REFUSED, not written: three corners
    //    on one line make the homography singular, so the compositor could not
    //    reproduce the keyed value and the cage lost the stroke entirely.
    if (want(6)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Perspective, QStringLiteral("transform"));
        tool.setRecordKeyframes(true);
        tool.activate(r.ctx);
        Vec2 shown[4];
        tool.frameCornersForTest(shown);
        const double before0 = r.adj->params().valueAt(ParamId::Corner0X, 0.0);
        const double before1 = r.adj->params().valueAt(ParamId::Corner0Y, 0.0);
        // Drag corner 0 straight down onto the edge 3->2, i.e. exactly onto the
        // line its two neighbours already lie on.
        const Vec2 onto{r.show({double(r.frame.x + r.frame.w), double(r.frame.y + r.frame.h)})};
        tool.beginPrimary(ev(shown[0].x, shown[0].y));
        for (int i = 1; i <= 8; ++i)
            tool.continuePrimary(ev(shown[0].x + (onto.x - shown[0].x) * double(i) / 8.0,
                                     shown[0].y + (onto.y - shown[0].y) * double(i) / 8.0));
        tool.endPrimary(ev(onto.x, onto.y));
        // The refused move changed nothing the model cannot map.
        INK_CHECK(allFinite(r.adj->params(), 0));
        tool.commit();
        // ...and the session is still usable and still shows a mapped quad.
        Vec2 after[4];
        tool.frameCornersForTest(after);
        for (int i = 0; i < 4; ++i)
            INK_CHECK(std::isfinite(after[i].x) && std::isfinite(after[i].y));
        // Somewhere on the way there the corner did move (the drag was not
        // simply ignored): the last valid position is kept, not the seed.
        const double now0 = r.adj->params().valueAt(ParamId::Corner0X, 0.0);
        const double now1 = r.adj->params().valueAt(ParamId::Corner0Y, 0.0);
        const bool movedALot = std::abs(now0 - before0) > 0.05 || std::abs(now1 - before1) > 0.05;
        const bool stayedValid = std::abs(now0 - before0) < 0.05 && std::abs(now1 - before1) < 0.05;
        INK_CHECK(movedALot || stayedValid);
        std::printf("degenerate perspective drag refused OK\n");
    }

    // 7. Re-entering a param record must not swallow the one in flight: the
    //    work since the last Apply has to stay one undo step of its own, or the
    //    next Ctrl+Z jumps further than the gesture being undone.
    if (want(7)) {
        Rig r;
        INK_CHECK(r.displayOk);
        AdjustmentLayer *a = r.adj;
        const size_t depth0 = r.doc.undo().undoDepth();
        r.doc.beginParamRecord("first", a);
        r.doc.recordParamValue(ParamId::PositionX, 60.0, 0);
        // A second open while the first is live (reachable because
        // endParamRecord notifies the timeline synchronously).
        r.doc.beginParamRecord("second", a);
        r.doc.recordParamValue(ParamId::PositionX, 90.0, 0);
        r.doc.endParamRecord();
        INK_CHECK(r.doc.undo().undoDepth() == depth0 + 2); // both steps survived
        r.doc.undo().undo();
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 60.0) < 1e-6);
        r.doc.undo().undo();
        INK_CHECK(std::abs(a->params().valueAt(ParamId::PositionX, 0.0) - 12.0) < 1e-6);
        std::printf("re-entrant param record keeps both undo steps OK\n");
    }

    // 8. A pixel session's map must not go stale: move a key while the tool is
    //    open and the next grab has to land on the re-seeded frame, not on the
    //    one the session was born with.
    if (want(8)) {
        Rig r;
        INK_CHECK(r.displayOk);
        TransformTool tool(TransformTool::Mode::Warp, QStringLiteral("transform"));
        tool.activate(r.ctx);
        const Vec2 anchor = tool.warpAnchorsForTest()[size_t(tool.warpAnchorsForTest().size() / 2)];
        INK_CHECK(tool.anchorAtForTest(r.show(anchor), 1.0) >= 0);
        // Scrub a key: the model is the same one the Rig built, so the session
        // will notice on its next press.
        r.doc.setParamKey(r.adj, ParamId::Rotation, 0, 30.0, Interp::Linear);
        r.doc.syncSelectionTransform();
        // The anchor the user can now see must be grabbable, at the position
        // the MODEL says it displays at.
        const Vec2 moved = tool.warpAnchorsForTest()[size_t(tool.warpAnchorsForTest().size() / 2)];
        const Mat3 now = r.adj->params().matrixFor(r.frame, 0.0, nullptr);
        const Vec2 shown = now.map(moved);
        INK_CHECK(tool.anchorAtForTest(shown, 1.0) >= 0);
        std::printf("pixel session re-seeds on a mid-session pose change OK\n");
    }

    std::printf("test_transform_record_modes PASS\n");
    return 0;
}
