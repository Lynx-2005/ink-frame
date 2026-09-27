// EVERY tool against a layer that has keyframes. The contract under test is the
// one that decides whether a keyframe makes an app feel broken:
//
//   * With "Record as keyframes" OFF, every tool does exactly what it did
//     before keyframes existed: it edits PIXELS. A keyed layer must not change
//     that, silently, behind the user's back.
//   * With it ON, the two tools that CAN record (Move, Transform) write keys
//     and leave the pixels byte-identical. Every other tool has nothing to
//     record - a brush stroke, a fill and a lasso are not transform channels -
//     so they keep baking. A tool must never half-record.
//   * Either way: no crash, one undo step per gesture, and undo puts the cel
//     back byte-for-byte.
//
// The keyed layer here carries a ROTATION, not just a translation, because a
// translation is the one pose a naive display mapping can represent - that gap
// is what let a family of keyframe bugs ship (see test_transform_record_modes).
#include "keyframe_harness.h"
#include "ui/tools/InkColorPickerTool.h"
#include "ui/tools/InkFillTool.h"
#include "ui/tools/InkMoveTool.h"
#include "ui/tools/InkPaintTool.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkShapeTools.h"
#include "ui/tools/InkTransformTool.h"
#include <memory>

using namespace ink;
using namespace ink::t;

namespace {

// One tool's gesture, described so the matrix below can run every tool through
// the same bake/record/no-crash checks.
struct Case {
    const char *name;
    std::unique_ptr<Tool> (*make)(bool record);
    // Where the gesture starts and ends, in DISPLAY space. Chosen to land on
    // the ink (or, for the selection tools, to enclose it) whatever the pose.
    Vec2 from, to;
    bool expectsPixels;  // the tool edits pixels when it is not recording
    bool canRecord;      // the tool has channels to record
};

// A pose that is unmistakably not a translation: 90 degrees plus a nudge, so a
// tool that maps the pointer with a Vec2 offset aims somewhere else entirely.
void keyRotated(Rig &r) {
    r.keyLayer();
    r.doc.setParamKey(r.adj, ParamId::Rotation, 0, 90.0, Interp::Linear);
    r.doc.setParamKey(r.adj, ParamId::PositionX, 0, 12.0, Interp::Linear);
    r.doc.syncSelectionTransform();
}

std::unique_ptr<Tool> brush(bool) { return std::make_unique<BrushTool>(); }
std::unique_ptr<Tool> eraser(bool) { return std::make_unique<EraserTool>(); }
std::unique_ptr<Tool> fill(bool) { return std::make_unique<FillTool>(); }
std::unique_ptr<Tool> line(bool) { return std::make_unique<LineTool>(); }
std::unique_ptr<Tool> rect(bool) { return std::make_unique<RectangleTool>(); }
std::unique_ptr<Tool> ellipse(bool) { return std::make_unique<EllipseTool>(); }
std::unique_ptr<Tool> picker(bool) { return std::make_unique<ColorPickerTool>(); }
std::unique_ptr<Tool> moveTool(bool record) {
    auto t = std::make_unique<MoveTool>();
    t->setRecordKeyframes(record);
    return t;
}
std::unique_ptr<Tool> transformFree(bool record) {
    auto t = std::make_unique<TransformTool>(TransformTool::Mode::Free, QStringLiteral("transform"));
    t->setRecordKeyframes(record);
    return t;
}

const std::vector<Case> &cases() {
    static const std::vector<Case> kCases = {
        {"brush", brush, {70, 60}, {95, 85}, true, false},
        {"eraser", eraser, {70, 60}, {95, 85}, true, false},
        {"fill", fill, {70, 60}, {70, 60}, true, false},
        {"line", line, {55, 55}, {110, 110}, true, false},
        {"rectangle", rect, {55, 55}, {110, 110}, true, false},
        {"ellipse", ellipse, {55, 55}, {110, 110}, true, false},
        {"picker", picker, {70, 60}, {95, 85}, false, false},
        {"move", moveTool, {70, 60}, {100, 90}, true, true},
        {"transform-free", transformFree, {70, 60}, {100, 90}, true, true},
    };
    return kCases;
}

// The selection tools are driven separately: they do not paint, they change the
// mask, and "did it work" is a question about Selection, not about pixels.
// INK_CHECK returns 1, so these helpers report through their return value and
// main() propagates it.
int selectionToolCases(const std::function<std::unique_ptr<Tool>()> &make, const char *name,
                       bool closedPath) {
    for (int rotate : {0, 90}) {
        Rig r;
        r.keyLayer();
        if (rotate) {
            r.doc.setParamKey(r.adj, ParamId::Rotation, 0, 90.0, Interp::Linear);
            r.doc.syncSelectionTransform();
        }
        auto tool = make();
        tool->activate(r.ctx);
        // A region drawn in DISPLAY space around the displayed ink. The lasso
        // needs a CLOSED path - a straight drag encloses no area, so an empty
        // selection there is the tool behaving correctly, not a bug.
        const Vec2 tl = r.show({double(kInk.x), double(kInk.y)});
        const Vec2 tr = r.show({double(kInk.x + kInk.w), double(kInk.y)});
        const Vec2 br = r.show({double(kInk.x + kInk.w), double(kInk.y + kInk.h)});
        const Vec2 bl = r.show({double(kInk.x), double(kInk.y + kInk.h)});
        std::vector<Vec2> path{bl, tl, tr, br, bl};
        if (!closedPath)
            path = {tl, br};
        tool->beginPrimary(ev(path.front().x, path.front().y));
        for (size_t i = 1; i < path.size(); ++i) {
            const Vec2 a = path[i - 1], b = path[i];
            for (int k = 1; k <= 3; ++k)
                tool->continuePrimary(ev(a.x + (b.x - a.x) * double(k) / 3.0,
                                         a.y + (b.y - a.y) * double(k) / 3.0));
        }
        tool->endPrimary(ev(path.back().x, path.back().y));
        // The mask is stored in DEVICE space, so it must land on the cel's own
        // ink rectangle whichever way the layer displays.
        const Rect b = r.doc.selection().bounds();
        INK_CHECK(!b.isEmpty());
        INK_CHECK(std::abs(b.x - kInk.x) <= 2 && std::abs(b.y - kInk.y) <= 2);
        INK_CHECK(std::abs(b.w - kInk.w) <= 3 && std::abs(b.h - kInk.h) <= 3);
        // ...and when the layer really is posed, the mask is handed the model's
        // own matrix to DISPLAY through, so the ants sit on the ink the user can
        // see. An identity pose has nothing to follow and must NOT claim to be
        // transformed (that would cost a mapping on every selection for nothing).
        if (rotate) {
            INK_CHECK(r.doc.selection().offsetActive());
            const Mat3 want = r.adj->params().matrixFor(r.frame, 0.0, nullptr);
            const Mat3 got = r.doc.selection().documentMatrix();
            for (int i = 0; i < 9; ++i)
                INK_CHECK(std::abs(got.m[i] - want.m[i]) < 1e-6);
        } else {
            INK_CHECK(!r.doc.selection().offsetActive());
        }
        tool->deactivate();
        r.doc.selection().reset(kW, kH);
        std::printf("%s selection follows the displayed ink (rot=%d) OK\n", name, rotate);
    }
    return 0;
}

// One tool, one gesture, on a keyed layer, with Record OFF and then ON.
int runCase(const Case &c, int variant) {
    Rig r;
    keyRotated(r);
    // Place the gesture relative to where the ink DISPLAYS, so every tool aims
    // at the stroke the user would actually be aiming at.
    const Vec2 from{double(kInk.x) + 20.0, double(kInk.y) + 10.0};
    const Vec2 to{double(kInk.x) + 55.0, double(kInk.y) + 30.0};
    const Vec2 dFrom = r.show(from);
    const Vec2 dTo = r.show(to);

    const std::vector<uint8_t> before = snapCel(r.doc, r.layer, 0);
    const int keysBefore = r.keys();
    const std::vector<double> valuesBefore = r.values(0);
    const size_t undoBefore = r.doc.undo().undoDepth();

    auto tool = c.make(variant == 1);
    tool->activate(r.ctx);
    tool->beginPrimary(ev(dFrom.x, dFrom.y));
    for (int i = 1; i <= 6; ++i) {
        ToolEvent e = ev(dFrom.x + (dTo.x - dFrom.x) * double(i) / 6.0,
                         dFrom.y + (dTo.y - dFrom.y) * double(i) / 6.0);
        e.timeSec = double(i) * 0.05;
        tool->continuePrimary(e);
    }
    tool->endPrimary(ev(dTo.x, dTo.y));
    tool->deactivate();

    const std::vector<uint8_t> after = snapCel(r.doc, r.layer, 0);
    INK_CHECK(allFinite(r.adj->params(), 0));

    const bool keysMoved = r.values(0) != valuesBefore;
    if (variant == 0) {
        // Record OFF: pixels are the medium and the model is untouched. A keyed
        // layer must not change that silently, behind the user's back.
        if (c.expectsPixels)
            INK_CHECK(before != after);
        INK_CHECK(!keysMoved);
        INK_CHECK(r.keys() == keysBefore);
    } else if (c.canRecord) {
        // Record ON: the tool records and leaves the cel byte-identical. Values,
        // not the key COUNT: keyCurrentLayer already animated all 18 channels,
        // so a record over an existing key updates it in place.
        INK_CHECK(before == after);
        INK_CHECK(keysMoved);
    } else {
        // Record ON but nothing to record: brush strokes, fills, shapes and
        // lassoes are not transform channels, so they keep baking and must not
        // touch a single key. Half-recording is the failure mode that matters.
        if (c.expectsPixels)
            INK_CHECK(before != after);
        INK_CHECK(!keysMoved);
        INK_CHECK(r.keys() == keysBefore);
    }

    // One gesture, one undo step, and undo restores the cel exactly. This is
    // the half that catches a tool that edits pixels AND records keys at once.
    // A tool that changed nothing at all (the colour picker samples, it does
    // not edit) must leave the undo stack alone: an undo entry for "nothing"
    // is how a stray Ctrl+Z starts eating the user's real work.
    const size_t depth = r.doc.undo().undoDepth();
    const bool changedSomething = (before != after) || keysMoved;
    if (changedSomething) {
        INK_CHECK(depth == undoBefore + 1);
        r.doc.undo().undo();
        INK_CHECK(snapCel(r.doc, r.layer, 0) == before);
        INK_CHECK(r.keys() == keysBefore);
        INK_CHECK(r.values(0) == valuesBefore);
    } else {
        INK_CHECK(depth == undoBefore);
    }
    tool.reset();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 1. The matrix: every painting/transform tool, Record off then on.
    for (const Case &c : cases()) {
        for (int variant = 0; variant < 2; ++variant) {
            INK_CHECK(runCase(c, variant) == 0);
        }
        std::printf("%-14s bakes and records per contract OK\n", c.name);
    }

    // 2. The selection tools, on a rotated and an unrotated keyed layer.
    INK_CHECK(selectionToolCases([] { return std::make_unique<RectSelectTool>(); }, "rect_select", false) == 0);
    INK_CHECK(selectionToolCases([] { return std::make_unique<EllipseSelectTool>(); }, "ellipse_select", false) == 0);
    INK_CHECK(selectionToolCases([] { return std::make_unique<LassoSelectTool>(); }, "lasso", true) == 0);

    // 3. A tool that cannot edit at all must be inert, not crash: a locked layer
    //    is the everyday way to find that out.
    {
        Rig r;
        keyRotated(r);
        r.layer->setLocked(true);
        const std::vector<uint8_t> before = snapCel(r.doc, r.layer, 0);
        const int keysBefore = r.keys();
        for (int variant = 0; variant < 2; ++variant) {
            for (const Case &c : cases()) {
                if (!c.expectsPixels)
                    continue;
                auto tool = c.make(variant == 1);
                tool->activate(r.ctx);
                tool->beginPrimary(ev(70, 60));
                tool->continuePrimary(ev(80, 70));
                tool->endPrimary(ev(90, 80));
                tool->deactivate();
            }
        }
        INK_CHECK(snapCel(r.doc, r.layer, 0) == before);
        INK_CHECK(r.keys() == keysBefore);
        std::printf("locked layer: every tool stays inert OK\n");
    }

    // 4. No exposed cell with Auto-key OFF: the "you cannot paint here" path.
    //    Tools must report it, not invent a cell and not key anything.
    {
        Rig r;
        r.doc.setCurrentFrame(5);
        r.doc.setAutoKeyEnabled(false);
        r.keyLayer();
        const int keysBefore = r.keys();
        for (int variant = 0; variant < 2; ++variant) {
            auto tool = transformFree(variant == 1);
            tool->activate(r.ctx);
            tool->beginPrimary(ev(70, 60));
            tool->continuePrimary(ev(90, 80));
            tool->endPrimary(ev(100, 90));
            tool->deactivate();
        }
        INK_CHECK(r.keys() == keysBefore);
        std::printf("no exposed cell: transform stays out of the way OK\n");
    }

    std::printf("test_keyframe_tool_matrix PASS\n");
    return 0;
}
