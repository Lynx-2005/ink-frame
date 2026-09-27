// The PERSPECTIVE tool against keyframed layers, in depth.
//
// Perspective is the one transform mode that owns FOUR channels the others do
// not (Corner0..3 X/Y) and the one whose state lives in FRAME space while the
// ink displays through the animation's matrix. Every other transform mode had a
// suite; this is perspective's, and it exists because the earlier coverage was
// one gesture on a rotation and nothing else.
//
// The contracts, all pose-independent and all checked against the MODEL rather
// than against the tool:
//
//   1. A corner you drag lands EXACTLY under the pointer, in both record and
//      pixel mode, under every pose - none, translate, rotate, scale, shear and
//      a projective (corner-keyed) pose. The cage is drawn through the same map
//      the compositor uses, so this can only hold if the drag math and the
//      drawing agree.
//   2. A body drag translates the whole cage by exactly the pointer delta, for
//      any pose: the tool measures the pointer in the space its state lives in
//      and the display maps it back out, so the two cancel.
//   3. An edge-midpoint drag moves both of its corners by the pointer delta and
//      leaves the other two alone.
//   4. In record mode the keyed corner is the frame-normalised point under the
//      pointer, taken through the pose's AFFINE map (the quad is the tool's own
//      state, so the affine is what carries a frame-space point to the screen).
//   5. In pixel mode the cel's pixels move and not one key is touched; in record
//      mode not one pixel is touched.
//   6. Esc reverts, Apply lands, and either way the session survives.
//   7. Alt re-places the pivot, which keys the anchor - and only the anchor.
//   8. Ctrl+corner, the "detach this corner" gesture, is pixel-only: in record
//      mode it must refuse rather than write a value the model cannot hold.
#include "keyframe_harness.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkTransformTool.h"
#include <algorithm>
#include <memory>

using namespace ink;
using namespace ink::t;

namespace {

// The pose's map WITHOUT the quad: the affine the compositor applies on top of
// the perspective corners. A copy of the block with the corner channels dropped
// back to their defaults, so matrixFor sees no override - derived from the model,
// never from the tool.
Mat3 affineOnly(const ParamBlock &in, const Rect &frame, int at) {
    ParamBlock b = in;
    static const ParamId kCorners[] = {ParamId::Corner0X, ParamId::Corner0Y, ParamId::Corner1X,
                                       ParamId::Corner1Y, ParamId::Corner2X, ParamId::Corner2Y,
                                       ParamId::Corner3X, ParamId::Corner3Y};
    for (ParamId id : kCorners) {
        for (int f : b.keys(id))
            b.removeKey(id, f);
        b.setStaticValue(id, paramDesc(id).def);
    }
    return b.matrixFor(frame, double(at), nullptr);
}

Mat3 invert(const Mat3 &m) {
    bool ok = false;
    const Mat3 r = m.inverted(&ok);
    return ok ? r : Mat3::identity();
}

// The gesture: press on `from`, drag by (dx,dy) in DISPLAY space, release.
void drag(Tool &tool, Vec2 from, double dx, double dy, Qt::KeyboardModifiers mods = Qt::NoModifier,
          int steps = 6) {
    ToolEvent press = ev(from.x, from.y);
    press.mods = mods;
    tool.beginPrimary(press);
    for (int i = 1; i <= steps; ++i) {
        ToolEvent e = ev(from.x + dx * double(i) / steps, from.y + dy * double(i) / steps);
        e.mods = mods;
        e.timeSec = double(i) * 0.05;
        tool.continuePrimary(e);
    }
    ToolEvent up = ev(from.x + dx, from.y + dy);
    up.mods = mods;
    tool.endPrimary(up);
}

// A pose to test under. `apply` keys whatever it needs on `adj`.
struct Pose {
    const char *name;
    void (*apply)(AdjustmentLayer *, int);
    // How exactly a BODY drag must move the drawn cage. An AFFINE pose maps a
    // translated source quad to a translated drawn quad, so every corner lands on
    // (its old spot + the pointer delta) to the last bit. A PROJECTIVE pose
    // genuinely does not: the perspective divide makes the displayed offset vary
    // across the frame, which is what perspective IS. So the exactness is
    // asserted where it is a mathematical identity, and a small tolerance where
    // the tool is only approximately right - rather than pretending the two are
    // the same contract.
    double bodyTol;
};

void poseNone(AdjustmentLayer *, int) {}

void poseTranslate(AdjustmentLayer *a, int) {
    // A translation is the one pose a naive display mapping CAN represent, so it
    // is here as the control: if this fails, the failure is not about rotations.
    static_cast<void>(a);
}

void poseRotate(AdjustmentLayer *a, int) {
    a->params().setKey(ParamId::Rotation, 0, 37.0f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionX, 0, 14.0f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionY, 0, -9.0f, Interp::Linear, true);
}

void poseScale(AdjustmentLayer *a, int) {
    a->params().setKey(ParamId::ScaleX, 0, 1.45f, Interp::Linear, true);
    a->params().setKey(ParamId::ScaleY, 0, 0.7f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionX, 0, 20.0f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionY, 0, 11.0f, Interp::Linear, true);
}

void poseShear(AdjustmentLayer *a, int) {
    a->params().setKey(ParamId::ShearX, 0, 0.55f, Interp::Linear, true);
    a->params().setKey(ParamId::ShearY, 0, -0.3f, Interp::Linear, true);
    a->params().setKey(ParamId::Rotation, 0, 18.0f, Interp::Linear, true);
}

// The hard one: the pose ITSELF carries a perspective quad, so the display map
// is projective and mapping a pointer back means inverting a homography.
void poseProjective(AdjustmentLayer *a, int) {
    a->params().setKey(ParamId::Rotation, 0, 25.0f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionX, 0, 8.0f, Interp::Linear, true);
    a->params().setKey(ParamId::Corner0X, 0, 0.08f, Interp::Linear, true);
    a->params().setKey(ParamId::Corner2Y, 0, 0.86f, Interp::Linear, true);
    a->params().setKey(ParamId::Corner3X, 0, 0.12f, Interp::Linear, true);
}

// A gentle pose for blocks that MEASURE the composited ink: a big one pushes the
// drawing off the page, and a clipped drawing's visible centre is no longer the
// mapped centre - which would make the measurement wrong rather than the tool.
void poseMild(AdjustmentLayer *a, int) {
    a->params().setKey(ParamId::ScaleX, 0, 1.15f, Interp::Linear, true);
    a->params().setKey(ParamId::ScaleY, 0, 0.95f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionX, 0, 4.0f, Interp::Linear, true);
    a->params().setKey(ParamId::PositionY, 0, 3.0f, Interp::Linear, true);
    a->params().setKey(ParamId::Rotation, 0, 8.0f, Interp::Linear, true);
}

const std::vector<Pose> &poses() {
    static const std::vector<Pose> kPoses = {{"none", poseNone, 1e-3},
                                             {"translate", poseTranslate, 1e-3},
                                             {"rotate", poseRotate, 1e-3},
                                             {"scale", poseScale, 1e-3},
                                             {"shear", poseShear, 1e-3},
                                             {"projective", poseProjective, 8.0}};
    return kPoses;
}

// A keyed rig in a given pose, ready for a perspective session.
// Big enough that a point can be clear of BOTH the pivot and the edges: with a
// 50px box no such point exists (a 9px pivot radius plus a 9px edge margin need
// half the box), so a "drag the body" test would silently grab the pivot
// instead - correct tool behaviour, wrong gesture.
const Rect kBigInk{55, 55, 90, 90};

struct Rigged {
    Rig r{kBigInk};
    Rect frame{};

    Rigged(const Pose &p) { setup(p.apply); }
    // A bare pose function is the common case in the single-pose blocks below.
    Rigged(void (*apply)(AdjustmentLayer *, int)) { setup(apply); }

    void setup(void (*apply)(AdjustmentLayer *, int)) {
        r.keyLayer();
        frame = r.frame;
        apply(r.adj, 0);
        // The model is the only source of truth for where the ink displays.
        r.doc.syncSelectionTransform();
    }
};

std::unique_ptr<TransformTool> perspective(Rig &r, bool record) {
    auto t = std::make_unique<TransformTool>(TransformTool::Mode::Perspective,
                                             QStringLiteral("transform"));
    t->setRecordKeyframes(record);
    t->activate(r.ctx);
    return t;
}

bool near(Vec2 a, Vec2 b, double tol) { return std::hypot(a.x - b.x, a.y - b.y) <= tol; }

// The bounding box of the SOLID part of a projected frame (alpha at or above a
// threshold). Two reasons this is the right instrument for "where did the ink
// end up":
//   - a filtered resample leaves a faint halo a pixel or two wide, and a
//     content-bounds measurement counts it, so the edges read a few pixels wide;
//   - for a CONVEX quad the extreme points in x and y ARE at the corners, so the
//     model's own four mapped corners predict the extremes exactly. (The area
//     centroid would NOT: it is not the image of the centre for a skewed quad,
//     which is why a centroid comparison only agrees when the quad happens to be
//     a parallelogram.)
Rect solidBounds(const std::vector<uint8_t> &px, int threshold = 128) {
    int x0 = kW, y0 = kH, x1 = -1, y1 = -1;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            if (px[(size_t(y) * kW + x) * 4 + 3] < threshold)
                continue;
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    return x1 < 0 ? Rect{} : Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// A point guaranteed to be grabbed as the BODY: clear of the pivot at the
// centre and clear of all four edges. Found by walking from the centre towards a
// corner - a convex quad keeps the segment inside, and both margins grow as the
// point approaches the corner - and stopping at the last offset that satisfies
// both. Returns false if the box is too small to offer one.
bool bodyPoint(const Vec2 c[4], double zoom, Vec2 *out) {
    const double r = 9.0 / std::max(0.05, zoom);  // the tool's handle radius
    const double margin = r * 1.8;                // comfortable clearance
    const Vec2 centre{(c[0].x + c[2].x) * 0.5, (c[0].y + c[2].y) * 0.5};
    const Vec2 dir{c[0].x - centre.x, c[0].y - centre.y};
    const double len = std::hypot(dir.x, dir.y);
    if (len < 1e-6)
        return false;
    const Vec2 step{dir.x / len, dir.y / len};
    for (double t = len * 0.9; t > 0.0; t -= len * 0.05) {
        const Vec2 p{centre.x + step.x * t, centre.y + step.y * t};
        // clear of the pivot?
        if (std::hypot(p.x - centre.x, p.y - centre.y) < margin)
            continue;
        // clear of every edge?
        double worst = 1e9;
        for (int i = 0; i < 4; ++i) {
            const Vec2 &a = c[i], &b = c[(i + 1) % 4];
            const double ex = b.x - a.x, ey = b.y - a.y;
            const double l2 = ex * ex + ey * ey;
            if (l2 < 1e-9)
                continue;
            double t2 = ((p.x - a.x) * ex + (p.y - a.y) * ey) / l2;
            t2 = std::clamp(t2, 0.0, 1.0);
            worst = std::min(worst, std::hypot(p.x - (a.x + ex * t2), p.y - (a.y + ey * t2)));
        }
        if (worst >= margin) {
            *out = p;
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 1. THE MATRIX: six poses x {record, pixel} x {corner, body, edge}. The
    //    contract is where things LAND, which is pose-independent.
    for (const Pose &pose : poses()) {
        for (int record = 0; record < 2; ++record) {
            for (int grip = 0; grip < 3; ++grip) {
                Rigged rig(pose);
                Rig &r = rig.r;
                const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
                const std::vector<double> keysBefore = r.values(0);

                auto tool = perspective(r, record == 1);
                Vec2 before[4];
                tool->frameCornersForTest(before);
                INK_CHECK(snapOk(celBefore));
                // A grip point that is unambiguous for each kind: a corner, a
                // body point well away from every handle, an edge midpoint.
                Vec2 from{0, 0};
                int corner = -1;
                if (grip == 0) {
                    corner = 1; // top-right
                    from = before[1];
                } else if (grip == 1) {
                    INK_CHECK(bodyPoint(before, 1.0, &from));
                } else {
                    from = {(before[0].x + before[1].x) * 0.5, (before[0].y + before[1].y) * 0.5};
                }
                const double dx = 17.0, dy = -11.0;
                drag(*tool, from, dx, dy);
                Vec2 after[4];
                tool->frameCornersForTest(after);
                INK_CHECK(allFinite(r.block(), 0));

                if (grip == 0) {
                    // The dragged corner is under the pointer, exactly.
                    const Vec2 want{from.x + dx, from.y + dy};
                    INK_CHECK(near(after[corner], want, 1e-3));
                } else if (grip == 1) {
                    // The whole cage moved by the pointer delta. Exact for an
                    // affine pose, close for a projective one (see Pose::bodyTol).
                    for (int i = 0; i < 4; ++i)
                        INK_CHECK(near(after[i], {before[i].x + dx, before[i].y + dy}, pose.bodyTol));
                } else {
                    // The two corners of the dragged edge moved by the delta and
                    // the other two stayed: a perspective edge drag reshapes, it
                    // does not move the quad.
                    INK_CHECK(near(after[0], {before[0].x + dx, before[0].y + dy}, pose.bodyTol));
                    INK_CHECK(near(after[1], {before[1].x + dx, before[1].y + dy}, pose.bodyTol));
                    INK_CHECK(near(after[2], before[2], 1e-3));
                    INK_CHECK(near(after[3], before[3], 1e-3));
                }

                // Record mode: keys moved, pixels did not. Pixel mode: the
                // reverse. Never both, never neither.
                const std::vector<uint8_t> celAfter = snapCel(r.doc, r.layer, 0);
                const bool keysMoved = r.values(0) != keysBefore;
                if (record) {
                    INK_CHECK(celAfter == celBefore);
                    INK_CHECK(keysMoved);
                } else {
                    INK_CHECK(celAfter != celBefore);
                    INK_CHECK(!keysMoved);
                }
                tool->deactivate();
            }
        }
        std::printf("perspective: pose %-12s lands under the pointer in both modes OK\n", pose.name);
    }

    // 2. The keyed corner is the frame-normalised point under the pointer, taken
    //    through the pose's AFFINE map. Checked for every pose, because this is
    //    the value that ends up in the project file and is read back on reload.
    for (const Pose &pose : poses()) {
        Rigged rig(pose);
        Rig &r = rig.r;
        auto tool = perspective(r, true);
        Vec2 before[4];
        tool->frameCornersForTest(before);
        // Whatever the rig keyed for the corners (a projective pose keys some of
        // them), the drag must leave the ones it did not touch exactly as they
        // were - not "as 0".
        const std::vector<double> keysBefore = r.values(0);
        const Vec2 from = before[2];
        const Vec2 drop{from.x + 21.0, from.y + 13.0};
        drag(*tool, from, 21.0, 13.0);

        const Mat3 aff = affineOnly(r.block(), rig.frame, 0);
        const Vec2 wantFrame = invert(aff).map(drop);
        const double wantX = (wantFrame.x - rig.frame.x) / double(rig.frame.w);
        const double wantY = (wantFrame.y - rig.frame.y) / double(rig.frame.h);
        INK_CHECK(r.animated(ParamId::Corner2X));
        INK_CHECK(r.animated(ParamId::Corner2Y));
        // float storage round-trip: a thousandth of the frame, not a pixel.
        INK_CHECK(std::abs(r.val(ParamId::Corner2X, 0) - wantX) < 0.01);
        INK_CHECK(std::abs(r.val(ParamId::Corner2Y, 0) - wantY) < 0.01);
        // ...and the corners it did not touch kept their values exactly.
        INK_CHECK(std::abs(r.val(ParamId::Corner0X, 0) - keysBefore[9]) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner0Y, 0) - keysBefore[10]) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner1X, 0) - keysBefore[11]) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner3Y, 0) - keysBefore[16]) < 1e-6);
        // The drawn corner is under the pointer, so the picture agrees with the
        // parameter that produces it.
        Vec2 after[4];
        tool->frameCornersForTest(after);
        INK_CHECK(near(after[2], drop, 1e-3));
        tool->deactivate();
        std::printf("perspective: pose %-12s records the frame-space corner OK\n", pose.name);
    }

    // 3. Pixel mode on a keyed pose really moves the PICTURE. A body drag is a
    //    translation of the source quad, and an AFFINE pose carries that through
    //    to a translation of the composite, so the composited ink must shift by
    //    exactly the drag. Measured on the composite, not on the tool.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
        const std::vector<double> keysBefore = r.values(0);
        PaintDevice projBefore;
        r.doc.image().project(0, projBefore);
        const Rect inkBefore = projBefore.contentBounds();
        INK_CHECK(snapOk(celBefore) && !inkBefore.isEmpty());

        auto tool = perspective(r, false);
        Vec2 before[4];
        tool->frameCornersForTest(before);
        Vec2 from{};
        INK_CHECK(bodyPoint(before, 1.0, &from));
        const double dx = 40.0, dy = 0.0;
        drag(*tool, from, dx, dy);
        tool->commit();

        // The cel took the pixels; the model took nothing.
        INK_CHECK(snapOk(snapCel(r.doc, r.layer, 0)));
        INK_CHECK(snapCel(r.doc, r.layer, 0) != celBefore);
        INK_CHECK(r.values(0) == keysBefore);
        // And the composited ink moved right by the drag, in the same direction
        // and by the same amount: the pose carried the bake with it.
        PaintDevice projAfter;
        r.doc.image().project(0, projAfter);
        const Rect inkAfter = projAfter.contentBounds();
        INK_CHECK(!inkAfter.isEmpty());
        INK_CHECK(std::abs((inkAfter.x - inkBefore.x) - dx) <= 2);
        INK_CHECK(std::abs(inkAfter.y - inkBefore.y) <= 2);
        // Unchanged VERTICAL extent: a body drag translates, it does not scale.
        // The width deliberately moves the ink off the right edge of the page -
        // the pose is large - so it is measured on the axis that has room. The
        // few-pixel tolerance is the filtered resample's antialiased fringe,
        // which a content-bounds measurement counts.
        INK_CHECK(std::abs(inkAfter.y - inkBefore.y) <= 2);
        INK_CHECK(std::abs(inkAfter.h - inkBefore.h) <= std::max(3, inkBefore.h / 20));
        INK_CHECK(allFinite(r.block(), 0));
        tool->deactivate();
        std::printf("perspective: pixel mode on a keyed pose moves the composite OK\n");
    }

    // 4. Zoom. Handles are a constant size on SCREEN, so their document radius
    //    divides by the zoom - a hit test that ignores that misses at any zoom
    //    but 1, and a drag measured in the wrong space lands off the pointer.
    for (double zoom : {0.5, 2.0}) {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        auto tool = perspective(r, true);
        Vec2 before[4];
        tool->frameCornersForTest(before);
        const Vec2 from = before[3];
        const Vec2 drop{from.x + 13.0, from.y - 7.0};
        // Drive the gesture at this zoom, in DOCUMENT coordinates.
        ToolEvent press = ev(from.x, from.y, zoom);
        tool->beginPrimary(press);
        for (int i = 1; i <= 5; ++i) {
            ToolEvent e = ev(from.x + 13.0 * double(i) / 5.0, from.y - 7.0 * double(i) / 5.0, zoom);
            e.timeSec = double(i) * 0.05;
            tool->continuePrimary(e);
        }
        tool->endPrimary(ev(drop.x, drop.y, zoom));
        Vec2 after[4];
        tool->frameCornersForTest(after);
        INK_CHECK(near(after[3], drop, 1e-3));
        INK_CHECK(allFinite(r.block(), 0));
        tool->deactivate();
        std::printf("perspective: zoom %.1f grab lands under the pointer OK\n", zoom);
    }

    // 5. Esc reverts; Apply lands. Both leave a live session, and neither can be
    //    half-applied.
    for (int record = 0; record < 2; ++record) {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
        const std::vector<double> keysBefore = r.values(0);
        auto tool = perspective(r, record == 1);
        Vec2 c[4];
        tool->frameCornersForTest(c);
        drag(*tool, c[0], 25.0, 15.0);
        if (record) {
            // isEditing() is "uncommitted PIXELS sit on the device" - it feeds
            // the canvas' cache-warming hint, and a record session has no
            // pending pixels by design. What is pending here is KEY work: the
            // record is open and the model already holds it, uncommitted.
            INK_CHECK(!tool->isEditing());
            INK_CHECK(r.ctx.recordActive());
            INK_CHECK(r.values(0) != keysBefore);
        } else {
            INK_CHECK(tool->isEditing()); // a pending preview
        }
        tool->cancelPrimary();
        // Reverted: exactly the state before the drag, and no undo entry for a
        // gesture the user threw away.
        INK_CHECK(snapCel(r.doc, r.layer, 0) == celBefore);
        INK_CHECK(r.values(0) == keysBefore);
        // ...and the session is still live, showing the reverted pose.
        Vec2 back[4];
        tool->frameCornersForTest(back);
        INK_CHECK(near(back[0], c[0], 1e-3));

        // Now do it again and Apply.
        drag(*tool, c[0], 25.0, 15.0);
        tool->commit();
        INK_CHECK(!tool->isEditing());
        if (record)
            INK_CHECK(snapCel(r.doc, r.layer, 0) == celBefore);
        else
            INK_CHECK(snapCel(r.doc, r.layer, 0) != celBefore);
        INK_CHECK(allFinite(r.block(), 0));
        tool->deactivate();
        std::printf("perspective: %s Esc reverts and Apply lands OK\n", record ? "record" : "pixel");
    }

    // 6. Alt re-places the pivot, which keys the ANCHOR - and only the anchor.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        auto tool = perspective(r, true);
        const std::vector<double> keysBefore = r.values(0);
        // The map the tool will measure the press through - captured BEFORE the
        // gesture, because writing the anchor changes the pose, and therefore
        // changes that very map. Expecting the new pose to explain the old
        // gesture is a circular check.
        const Mat3 affBefore = affineOnly(r.block(), rig.frame, 0);
        // Press well away from the frame, with Alt: that is the pivot gesture.
        ToolEvent e = ev(20.0, 20.0);
        e.mods = Qt::AltModifier;
        tool->beginPrimary(e);
        ToolEvent move = ev(31.0, 27.0);
        move.mods = Qt::AltModifier;
        tool->continuePrimary(move);
        tool->endPrimary(move);

        // The anchor moved; the corners did not.
        INK_CHECK(std::abs(r.val(ParamId::AnchorX, 0) - keysBefore[2]) > 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner0X, 0) - keysBefore[9]) < 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner3Y, 0) - keysBefore[16]) < 1e-6);
        // The stored anchor is the frame-normalised point the user PLACED - and
        // Alt-click places at the press, like Krita, so the press is what counts
        // and the follow-up move is irrelevant.
        const Vec2 wantFrame = invert(affBefore).map({20.0, 20.0});
        INK_CHECK(std::abs(r.val(ParamId::AnchorX, 0) - (wantFrame.x - rig.frame.x) / rig.frame.w) < 0.01);
        INK_CHECK(std::abs(r.val(ParamId::AnchorY, 0) - (wantFrame.y - rig.frame.y) / rig.frame.h) < 0.01);
        INK_CHECK(allFinite(r.block(), 0));
        tool->deactivate();
        std::printf("perspective: Alt places the pivot and keys only the anchor OK\n");
    }

    // 7. Two frames, isolated, with undo. A perspective record at frame 4 must
    //    not disturb frame 0, and one Ctrl+Z must undo exactly that record.
    {
        Rigged rig(poseNone);
        Rig &r = rig.r;
        r.doc.setCurrentFrame(0);
        {
            auto tool = perspective(r, true);
            Vec2 c[4];
            tool->frameCornersForTest(c);
            drag(*tool, c[0], 18.0, 0.0);
            tool->commit();
        }
        const std::vector<double> at0 = r.values(0);
        r.doc.setCurrentFrame(4);
        {
            auto tool = perspective(r, true);
            Vec2 c[4];
            tool->frameCornersForTest(c);
            drag(*tool, c[2], 0.0, 22.0);
            tool->commit();
        }
        INK_CHECK(r.values(0) == at0);
        INK_CHECK(r.animated(ParamId::Corner0X) && r.animated(ParamId::Corner2Y));
        r.doc.undo().undo();
        INK_CHECK(r.values(0) == at0);
        INK_CHECK(allFinite(r.block(), 0) && allFinite(r.block(), 4));
        std::printf("perspective: two frames stay isolated and undo cleanly OK\n");
    }

    // 8. Ctrl is not a modifier in Perspective. "Detach this corner" is a Free
    //    gesture - Perspective is a quad mode already, so there is nothing to
    //    detach and Ctrl+corner must simply BE a corner drag: the corner follows
    //    the pointer and is keyed like any other. Free is where the pixel-only
    //    hazard lives, so that is checked there.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        const std::vector<double> keysBefore = r.values(0);
        const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
        auto tool = perspective(r, true);
        Vec2 c[4];
        tool->frameCornersForTest(c);
        const Vec2 drop{c[0].x + 30.0, c[0].y + 20.0};
        drag(*tool, c[0], 30.0, 20.0, Qt::ControlModifier);
        // A corner drag: keyed, no pixels, and the corner is under the pointer.
        INK_CHECK(snapCel(r.doc, r.layer, 0) == celBefore);
        INK_CHECK(r.values(0) != keysBefore);
        Vec2 after[4];
        tool->frameCornersForTest(after);
        INK_CHECK(near(after[0], drop, 1e-3));
        tool->deactivate();
        std::printf("perspective: Ctrl+corner is a plain corner drag OK\n");
    }

    // 8b. FREE's Ctrl+corner detaches the corner into a live quad - and the
    //     record path writes only the affine channels, so such a quad would
    //     silently vanish at commit. In record mode it must refuse and touch
    //     nothing; in pixel mode the same gesture must actually distort.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        const std::vector<double> keysBefore = r.values(0);
        const std::vector<uint8_t> celBefore = snapCel(r.doc, r.layer, 0);
        {
            TransformTool freeTool(TransformTool::Mode::Free, QStringLiteral("transform"));
            freeTool.setRecordKeyframes(true);
            freeTool.activate(r.ctx);
            Vec2 c[4];
            freeTool.frameCornersForTest(c);
            drag(freeTool, c[0], 30.0, 20.0, Qt::ControlModifier);
            freeTool.commit();
            std::fprintf(stderr, "  DBG freeTool recordActive=%d editing=%d recording=%d\n",
                         int(r.ctx.recordActive()), int(freeTool.isEditing()), int(r.ctx.recording()));
        }
        INK_CHECK(r.values(0) == keysBefore);
        INK_CHECK(snapCel(r.doc, r.layer, 0) == celBefore);

        auto pix = std::make_unique<TransformTool>(TransformTool::Mode::Free,
                                                   QStringLiteral("transform"));
        pix->activate(r.ctx);
        Vec2 p[4];
        pix->frameCornersForTest(p);
        drag(*pix, p[0], 30.0, 20.0, Qt::ControlModifier);
        INK_CHECK(snapCel(r.doc, r.layer, 0) != celBefore);
        INK_CHECK(r.values(0) == keysBefore);
        pix->deactivate();
        std::printf("free: Ctrl+corner refuses to record and distorts when baking OK\n");
    }

    // 9. A selection-scoped session on a keyed layer. The quad is built from the
    //    SELECTION's contents, and the selection travels with the pose, so the
    //    cage must outline the selection the user sees.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        RectSelectTool sel;
        sel.activate(r.ctx);
        const Vec2 tl = r.show({30.0, 30.0});
        const Vec2 br = r.show({110.0, 110.0});
        sel.beginPrimary(ev(tl.x, tl.y));
        for (int i = 1; i <= 3; ++i)
            sel.continuePrimary(ev(tl.x + (br.x - tl.x) * double(i) / 3.0,
                                   tl.y + (br.y - tl.y) * double(i) / 3.0));
        sel.endPrimary(ev(br.x, br.y));
        sel.deactivate();
        INK_CHECK(!r.doc.selection().isEmpty());
        auto tool = perspective(r, false);
        Vec2 c[4];
        tool->frameCornersForTest(c);
        // The cage is a quad around the SELECTION, not the whole layer: smaller
        // than the layer's own content box.
        const Rect cage = [&] {
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            for (int i = 0; i < 4; ++i) {
                x0 = std::min(x0, c[i].x);
                y0 = std::min(y0, c[i].y);
                x1 = std::max(x1, c[i].x);
                y1 = std::max(y1, c[i].y);
            }
            return Rect{int(x0), int(y0), int(x1 - x0) + 1, int(y1 - y0) + 1};
        }();
        INK_CHECK(!cage.isEmpty());
        // It must be in the neighbourhood of the selection the user drew.
        const Rect sb = r.doc.selection().docBounds();
        INK_CHECK(cage.x > sb.x - 25 && cage.y > sb.y - 25);
        INK_CHECK(cage.x + cage.w < sb.x + sb.w + 25);
        INK_CHECK(cage.y + cage.h < sb.y + sb.h + 25);
        // A drag still lands under the pointer with a selection in play.
        const Vec2 drop{c[1].x + 12.0, c[1].y + 6.0};
        drag(*tool, c[1], 12.0, 6.0);
        Vec2 after[4];
        tool->frameCornersForTest(after);
        INK_CHECK(near(after[1], drop, 1e-3));
        tool->deactivate();
        r.doc.selection().reset(kW, kH);
        std::printf("perspective: selection-scoped cage on a keyed pose OK\n");
    }

    // 10. Switching out of perspective and back must not corrupt the session:
    //     the pending work lands, and the fresh perspective session is seeded
    //     from the pose the model now holds.
    {
        Rigged rig(poseRotate);
        Rig &r = rig.r;
        auto tool = perspective(r, true);
        Vec2 c[4];
        tool->frameCornersForTest(c);
        drag(*tool, c[0], 16.0, 10.0);
        tool->setMode(TransformTool::Mode::Free); // settles the pending record
        INK_CHECK(allFinite(r.block(), 0));
        tool->setMode(TransformTool::Mode::Perspective);
        INK_CHECK(allFinite(r.block(), 0));
        // The new session's cage agrees with the model.
        const Mat3 model = r.block().matrixFor(rig.frame, double(r.doc.currentFrame()), nullptr);
        Vec2 fc[4], drawn[4];
        rectCorners(rig.frame, fc);
        tool->frameCornersForTest(drawn);
        for (int i = 0; i < 4; ++i) {
            const Vec2 want = model.map(fc[i]);
            INK_CHECK(near(drawn[i], want, 1e-3));
        }
        tool->deactivate();
        std::printf("perspective: mode round-trip re-seeds from the model OK\n");
    }

    // 11. The picture the parameters describe must be the picture on screen:
    //     after a record the composited ink sits where the model's own matrix
    //     says, checked on the ink's bounds rather than on the tool.
    {
        Rigged rig({ "mild", poseMild, 1e-3 });
        Rig &r = rig.r;
        const std::vector<uint8_t> celUnmoved = snapCel(r.doc, r.layer, 0);
        const std::vector<double> keysBefore = r.values(0);
        {
            // BASELINE: with no gesture at all, does the composite already sit
            // where the model says? If not, the measurement - not the tool - is
            // what is wrong.
            const Mat3 m0 = r.block().matrixFor(rig.frame, 0.0, nullptr);
            Vec2 q[4];
            q[0] = m0.map({double(r.ink.x), double(r.ink.y)});
            q[1] = m0.map({double(r.ink.x + r.ink.w), double(r.ink.y)});
            q[2] = m0.map({double(r.ink.x + r.ink.w), double(r.ink.y + r.ink.h)});
            q[3] = m0.map({double(r.ink.x), double(r.ink.y + r.ink.h)});
            double mnx = 1e9, mny = 1e9, mxx = -1e9, mxy = -1e9;
            for (int i = 0; i < 4; ++i) {
                mnx = std::min(mnx, q[i].x);
                mny = std::min(mny, q[i].y);
                mxx = std::max(mxx, q[i].x);
                mxy = std::max(mxy, q[i].y);
            }
            const Rect g0 = solidBounds(snapCell(r.doc, 0));
            std::fprintf(stderr, "  DBG baseline model %.1f,%.1f..%.1f,%.1f  composite %d,%d..%d,%d\n",
                         mnx, mny, mxx, mxy, g0.x, g0.y, g0.x + g0.w - 1, g0.y + g0.h - 1);
            INK_CHECK(std::abs(double(g0.x) - mnx) <= 3.0);
            INK_CHECK(std::abs(double(g0.y) - mny) <= 3.0);
            INK_CHECK(std::abs(double(g0.x + g0.w - 1) - mxx) <= 3.0);
            INK_CHECK(std::abs(double(g0.y + g0.h - 1) - mxy) <= 3.0);
        }
        auto tool = perspective(r, true);
        Vec2 c[4];
        tool->frameCornersForTest(c);
        drag(*tool, c[3], -14.0, 18.0);
        tool->commit();
        INK_CHECK(snapOk(celUnmoved));
        // Model: the pose that now exists maps the ink's own box, and for a
        // convex quad its four extremes are at the mapped corners.
        const Mat3 model = r.block().matrixFor(rig.frame, 0.0, nullptr);
        const Rect inkRect = r.ink;
        Vec2 want[4];
        want[0] = model.map({double(inkRect.x), double(inkRect.y)});
        want[1] = model.map({double(inkRect.x + inkRect.w), double(inkRect.y)});
        want[2] = model.map({double(inkRect.x + inkRect.w), double(inkRect.y + inkRect.h)});
        want[3] = model.map({double(inkRect.x), double(inkRect.y + inkRect.h)});
        double mnx = 1e9, mny = 1e9, mxx = -1e9, mxy = -1e9;
        for (int i = 0; i < 4; ++i) {
            mnx = std::min(mnx, want[i].x);
            mny = std::min(mny, want[i].y);
            mxx = std::max(mxx, want[i].x);
            mxy = std::max(mxy, want[i].y);
        }
        const Rect got = solidBounds(snapCell(r.doc, 0));
        INK_CHECK(!got.isEmpty());
        std::fprintf(stderr, "  DBG after model %.1f,%.1f..%.1f,%.1f  composite %d,%d..%d,%d\n", mnx,
                     mny, mxx, mxy, got.x, got.y, got.x + got.w - 1, got.y + got.h - 1);
        INK_CHECK(std::abs(double(got.x) - mnx) <= 3.0);
        INK_CHECK(std::abs(double(got.y) - mny) <= 3.0);
        INK_CHECK(std::abs(double(got.x + got.w - 1) - mxx) <= 3.0);
        INK_CHECK(std::abs(double(got.y + got.h - 1) - mxy) <= 3.0);
        // The cel itself never moved: this whole picture came from the pose.
        INK_CHECK(snapCel(r.doc, r.layer, 0) == celUnmoved);
        // And only the corner the drag touched was keyed.
        INK_CHECK(std::abs(r.val(ParamId::Corner3X, 0) - keysBefore[15]) > 1e-6);
        INK_CHECK(std::abs(r.val(ParamId::Corner0X, 0) - keysBefore[9]) < 1e-9);
        tool->deactivate();
        std::printf("perspective: the composite matches the recorded pose OK\n");
    }

    std::printf("test_keyframe_perspective PASS\n");
    return 0;
}
