#pragma once
// Shared scaffolding for the keyframe x tool test suites.
//
// Every suite here drives REAL tools through the REAL ToolContext against a
// REAL Document, and checks the thing the user can actually see: the pixels on
// the cel, the keys in the model, and the COMPOSITE (what the canvas projects
// for a frame). Two rules the suites are built on, both learned the hard way:
//
//  1. An expectation is never derived from the value under test. Pose
//     expectations come from ParamBlock::matrixFor, display positions from that
//     matrix, the composite from Document::project. Asking the tool where it
//     thinks the ink is, and then agreeing with it, proves nothing - that is
//     exactly how a translation-only display mapping shipped.
//  2. "Nothing was touched" is asserted as a byte-equal snapshot, not as an
//     absence of exceptions. A tool that quietly painted a pixel has still
//     broken the contract.
#ifndef INK_TEST_KEYFRAME_HARNESS_H
#define INK_TEST_KEYFRAME_HARNESS_H

#include "../test_check.h"
#include "core/InkPaintDevice.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkTool.h"
#include "ui/tools/InkToolContext.h"
#include <QApplication>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ink::t {

constexpr int kW = 200;
constexpr int kH = 200;
// Off-centre so a page-anchored transform cannot pass by accident.
const Rect kInk{40, 40, 50, 50};

struct Host : ToolHost {
    double now{0.0};
    int repaints{0};
    int rebuilds{0};
    void repaint() override { ++repaints; }
    void rebuild() override { ++rebuilds; }
    // A host that has no clock reports 0, which the tools read as "never
    // throttle" - that is ToolHost's documented default and it keeps the
    // headless runs deterministic.
    double nowSec() const override { return now; }
};

inline ToolEvent ev(double x, double y, double zoom = 1.0) {
    ToolEvent e;
    e.docPos = {x, y};
    e.zoom = zoom;
    return e;
}

inline ToolEvent evMods(double x, double y, Qt::KeyboardModifiers mods) {
    ToolEvent e = ev(x, y);
    e.mods = mods;
    return e;
}

inline std::vector<uint8_t> snap(const PaintDevice &dev, int w = kW, int h = kH) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    const_cast<PaintDevice &>(dev).readRect({0, 0, w, h}, buf.data());
    return buf;
}

inline std::vector<uint8_t> snapCell(Document &doc, int frame) {
    PaintDevice scratch;
    doc.image().project(frame, scratch);
    return snap(scratch);
}

// The cel's own pixels for a FRAME. PaintLayer::deviceAt() takes a CELL KEY, not
// a frame - cellAt() is an exact map lookup with no span resolution - so a
// frame that merely falls inside a cell has no device of its own. Going through
// deviceAt(frame) directly therefore returns null and, worse, yields an EMPTY
// snapshot that compares equal to every other empty snapshot: a whole assertion
// silently passes. exposureAt() is the frame -> cell-key resolution.
inline std::vector<uint8_t> snapCel(Document &doc, PaintLayer *layer, int frame) {
    const int key = layer->exposureAt(frame);
    PaintDevice *dev = key >= 0 ? layer->deviceAt(key, false) : nullptr;
    if (!dev)
        return {};
    return snap(*dev);
}

// A snapshot that could not be taken is a FAILURE, not an empty picture. Tests
// assert on this so a null device can never masquerade as "nothing changed".
inline bool snapOk(const std::vector<uint8_t> &v) { return v.size() == size_t(kW) * kH * 4; }

// How many pixels in `r` differ between two snapshots (alpha or colour).
// -1 means "these two cannot be compared" (different sizes, or a snapshot that
// was never taken). Callers must treat that as a failure.
inline int diffCount(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, Rect r) {
    if (a.size() != b.size() || !snapOk(a) || !snapOk(b))
        return -1;
    r = intersectRect(r, Rect{0, 0, kW, kH});
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const size_t i = (size_t(y) * kW + x) * 4;
            if (a[i + 3] != b[i + 3] || a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
                ++n;
        }
    return n;
}

inline bool sameIn(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, Rect r) {
    return diffCount(a, b, r) == 0;
}

inline bool nonempty(const std::vector<uint8_t> &v) {
    for (size_t i = 3; i < v.size(); i += 4)
        if (v[i] != 0)
            return true;
    return false;
}

inline Rect around(Vec2 p, int r) { return {int(std::lround(p.x)) - r, int(std::lround(p.y)) - r, 2 * r + 1, 2 * r + 1}; }

// The COMPOSITE's idea of where a device point is displayed, straight from the
// model. Tests compare the tool against this, never against the tool.
struct Rig {
    Document doc{kW, kH, 12};
    ToolContext ctx{&doc};
    Host host;
    PaintLayer *layer{nullptr};
    AdjustmentLayer *adj{nullptr};
    Rect frame{};
    Rect ink{kInk};
    bool displayOk{false};

    // `inkRect` is the drawing, and therefore the lift, the frame and every
    // handle. It defaults to the small off-centre square; a suite that needs
    // room between the pivot and the edges (a 50px box cannot hold a point that
    // is clear of both) asks for a bigger one.
    explicit Rig(Rect inkRect = kInk) : ink(inkRect) {
        ctx.setHost(&host);
        GroupLayer *folder = doc.ensureFolder();
        layer = doc.createTrack(folder, "art");
        PaintDevice *dev = layer->deviceAt(0, true);
        dev->fillRect(ink, PixelRGBA8{40, 90, 200, 255}, CompositeOp::Normal);
        layer->setSpanEnd(0, kOpenEnd);
        doc.setCurrentLayer(layer);
        doc.setCurrentFrame(0);
        doc.setAutoKeyEnabled(true);
        frame = Rect{0, 0, kW, kH};
    }

    void keyLayer() {
        doc.keyCurrentLayer(0);
        adj = doc.ensureAdjustmentFor(layer);
        frame = block().frameOr(Rect{0, 0, kW, kH});
    }
    // The model's own map for `frame`, and its inverse. A keyed pose may
    // rotate, scale, shear or perspective-warp: a translation-only expectation
    // cannot express any of that, which is why every pose check goes through
    // this.
    Mat3 display(int at) const { return block().matrixFor(frame, double(at), nullptr); }
    Mat3 back(int at) const {
        bool ok = false;
        const Mat3 m = display(at).inverted(&ok);
        return ok ? m : Mat3::identity();
    }
    Vec2 show(Vec2 device, int at = 0) const { return display(at).map(device); }
    Vec2 hide(Vec2 displayPos, int at = 0) const { return back(at).map(displayPos); }
    // The DURABLE storage. Document keeps exactly ONE adjustment "view" alive
    // and destroys the previous one whenever the keyed layer changes
    // (ensureAdjustmentFor), so a cached AdjustmentLayer* dangles the moment
    // another layer is touched - and every assertion below would be reading
    // freed memory. The keys themselves live on the paint layer, so that is what
    // tests read; the view is only a handle to them.
    const ParamBlock &block() const { return layer->params(); }
    int keys() const { return layer->params().keyCount(); }
    // Every transform channel's value at `at`. A key COUNT is the wrong thing to
    // watch: keyCurrentLayer() animates all 18 channels up front, so recording
    // over an existing key UPDATES it and the count never moves. What a record
    // changes is the VALUES.
    std::vector<double> values(int at) const {
        static const ParamId kAll[] = {ParamId::PositionX, ParamId::PositionY,
                                       ParamId::AnchorX,    ParamId::AnchorY,
                                       ParamId::ScaleX,     ParamId::ScaleY,
                                       ParamId::Rotation,   ParamId::ShearX,
                                       ParamId::ShearY,     ParamId::Corner0X,
                                       ParamId::Corner0Y,   ParamId::Corner1X,
                                       ParamId::Corner1Y,   ParamId::Corner2X,
                                       ParamId::Corner2Y,   ParamId::Corner3X,
                                       ParamId::Corner3Y};
        std::vector<double> out;
        out.reserve(sizeof(kAll) / sizeof(kAll[0]));
        for (ParamId id : kAll)
            out.push_back(layer->params().valueAt(id, double(at)));
        return out;
    }
    double val(ParamId id, int at) const { return layer->params().valueAt(id, double(at)); }
    bool animated(ParamId id) const { return layer->params().isAnimated(id); }
};

// The first transform channel that evaluates to a non-finite number at `frame`,
// or ParamId::Count when the block is clean. Lets a failure NAME the channel
// instead of just reporting "some value is not a number".
inline ParamId firstNonFinite(const ParamBlock &b, int frame) {
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
            return id;
    return ParamId::Count;
}

inline bool allFinite(const ParamBlock &b, int frame) {
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

// Run one press/moves/release gesture. `steps` positions from `from` to `to`.
template <typename F> inline void drag(Tool &tool, F &ctx, Vec2 from, Vec2 to, int steps = 6,
                                        Qt::KeyboardModifiers mods = Qt::NoModifier) {
    tool.beginPrimary(evMods(from.x, from.y, mods));
    for (int i = 1; i <= steps; ++i) {
        const double t = double(i) / double(steps);
        ToolEvent e = evMods(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, mods);
        e.timeSec = double(i) * 0.05;
        tool.continuePrimary(e);
    }
    tool.endPrimary(evMods(to.x, to.y, mods));
    (void)ctx;
}

} // namespace ink::t

#endif
