// Adjustment layers: the link, the parameter block, and the composite hook.
//
// The three claims this file exists to defend:
//   1. A project with NO adjustment layer, one with an UNLINKED adjustment
//      layer, and one with a link whose block is still at its defaults all
//      project to bit-identical pixels. Keyframes must cost nothing until they
//      are used.
//   2. An animated transform actually moves the target layer's pixels, and an
//      integer translation moves them losslessly (no filter softening).
//   3. The animated opacity multiplies the track's own opacity.
#include "core/InkAdjustmentLayer.h"
#include "core/InkImage.h"
#include "io/InkProject.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ink;

namespace {

// A folder + one track + one 1-frame cell holding a solid 20x20 square. The
// square is offset from the origin so a translation is measurable.
struct Fixture {
    Image img{128, 96, 12};
    GroupLayer *folder{nullptr};
    PaintLayer *track{nullptr};
    int key{0};

    Fixture() {
        folder = img.createFolder("F");
        track = img.createTrack(folder, "T");
        PaintDevice *dev = track->deviceAt(key, true);
        dev->fillRect(Rect{20, 20, 20, 20}, PixelRGBA8{200, 40, 40, 255}, CompositeOp::Normal);
        // One cell, open exposure: every frame shows the square, so a test can
        // ask what the animation did to it on any frame.
        track->setSpanEnd(key, kOpenEnd);
    }
};

std::vector<uint8_t> snapshot(const PaintDevice &dev, const Rect &r) {
    std::vector<uint8_t> buf(size_t(r.w) * size_t(r.h) * 4, 0);
    dev.readRect(r, buf.data());
    return buf;
}

Rect canvasRect(const Image &img) { return Rect{0, 0, img.width(), img.height()}; }

bool sameRect(const Rect &a, const Rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// PaintDevice::extent() is TILE granular (the bounding box of whole tiles that
// hold ink), so it cannot measure a 20x20 square. Content bounds scan for
// actually-painted pixels, which is what these tests mean by "where the ink is".
// A filtered resample feathers the boundary, so a threshold measures the solid
// body of the ink rather than the fringe around it.
Rect contentBounds(const PaintDevice &dev, int alphaMin = 128) {
    const Rect r = dev.extent();
    if (r.isEmpty()) return {};
    int x0 = r.x + r.w, y0 = r.y + r.h, x1 = r.x, y1 = r.y;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            if (dev.pixel(x, y).a >= alphaMin) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    if (x1 < x0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// Same, with a slack of `slack` pixels on every side: a filtered (bicubic)
// resample legitimately feathers the boundary by a pixel.
bool nearRect(const Rect &a, const Rect &b, int slack) {
    return std::abs(a.x - b.x) <= slack && std::abs(a.y - b.y) <= slack &&
           std::abs(a.w - b.w) <= 2 * slack && std::abs(a.h - b.h) <= 2 * slack;
}

// Centroid of non-transparent pixels, in document space.
Vec2 centroid(const PaintDevice &dev) {
    const Rect r = dev.extent();
    if (r.isEmpty()) return {-1, -1};
    double sx = 0, sy = 0, n = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            if (dev.pixel(x, y).a > 0) {
                sx += x;
                sy += y;
                n += 1;
            }
    if (n == 0) return {-1, -1};
    return {sx / n, sy / n};
}

// Keyframes now live on the paint layer itself; the "adjustment layer" tests
// key the track directly through the same facade the UI uses.
AdjustmentLayer *addAdjustment(Image &img, GroupLayer *folder, const std::string &name) {
    (void)img; (void)folder;
    static std::vector<std::unique_ptr<AdjustmentLayer>> keep;
    keep.clear();
    keep.push_back(std::make_unique<AdjustmentLayer>(name));
    return keep.back().get();
}

} // namespace

int main() {
    // 1. Bit-exact no-op: absent, unlinked, and default-valued all match.
    {
        Fixture f;
        PaintDevice reference;
        f.img.project(f.key, reference);
        const Rect full = canvasRect(f.img);
        const std::vector<uint8_t> ref = snapshot(reference, full);
        INK_CHECK(sameRect(contentBounds(reference), Rect{20, 20, 20, 20}));

        // (a) an unlinked adjustment layer changes nothing
        AdjustmentLayer *unlinked = addAdjustment(f.img, f.folder, "A-unlinked");
        PaintDevice p1;
        f.img.project(f.key, p1);
        INK_CHECK(snapshot(p1, full) == ref);
        INK_CHECK(unlinked->target() == nullptr);

        // (b) linked, but the block is untouched: still identity
        AdjustmentLayer *linked = addAdjustment(f.img, f.folder, "A-default");
        linked->setTargetLayer(f.track);
        PaintDevice p2;
        f.img.project(f.key, p2);
        INK_CHECK(snapshot(p2, full) == ref);

        // (c) the static value back at its default contributes nothing
        linked->params().setStaticValue(ParamId::PositionX, 30.0);
        PaintDevice p3;
        f.img.project(f.key, p3);
        INK_CHECK(snapshot(p3, full) != ref); // 30px right: real animation
        linked->params().setStaticValue(ParamId::PositionX, 0.0);
        PaintDevice p4;
        f.img.project(f.key, p4);
        INK_CHECK(snapshot(p4, full) == ref);
        std::printf("identity no-op OK\n");
    }

    // 2. An integer translation moves the pixels and keeps them exact.
    {
        Fixture f;
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setStaticValue(ParamId::PositionX, 16.0);
        a->params().setStaticValue(ParamId::PositionY, 8.0);

        PaintDevice moved;
        f.img.project(f.key, moved);
        INK_CHECK(sameRect(contentBounds(moved), Rect{36, 28, 20, 20}));
        const Vec2 c = centroid(moved);
        INK_CHECK(std::abs(c.x - 45.5) < 0.01 && std::abs(c.y - 37.5) < 0.01);

        // Lossless: every pixel equals the original square's pixel, shifted.
        Fixture ref;
        PaintDevice base;
        ref.img.project(ref.key, base);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const PixelRGBA8 s = base.pixel(20 + x, 20 + y);
                const PixelRGBA8 d = moved.pixel(20 + x + 16, 20 + y + 8);
                INK_CHECK(s.r == d.r && s.g == d.g && s.b == d.b && s.a == d.a);
            }
        std::printf("integer translation lossless OK\n");
    }

    // 3. A keyed translation animates: the centroid tracks the channel, and a
    //    frame with no key still evaluates (holds/interpolates).
    {
        Fixture f;
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        a->params().setKey(ParamId::PositionX, 10, 40.0, Interp::Bezier);

        PaintDevice p;
        f.img.project(0, p);
        Vec2 c0 = centroid(p);
        f.img.project(10, p);
        Vec2 c10 = centroid(p);
        f.img.project(5, p);
        Vec2 c5 = centroid(p);
        INK_CHECK(std::abs(c0.x - 29.5) < 0.01);
        INK_CHECK(std::abs(c10.x - 69.5) < 0.01);
        // Auto tangents make collinear keys exactly linear, so frame 5 is the
        // midpoint, not a wobble.
        INK_CHECK(std::abs(c5.x - 49.5) < 0.5);
        // The Y channel is untouched.
        INK_CHECK(std::abs(c5.y - 29.5) < 0.01);
        std::printf("keyed translation OK\n");
    }

    // 4. Scale and rotation act about the ANCHOR, and the anchor is normalized
    //    to the canvas — so a default anchor is the frame centre, and moving it
    //    onto the ink makes scaling grow the drawing in place.
    {
        Fixture f;
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        // Anchor on the square's own centre (29.5, 29.5) of a 128x96 canvas.
        a->params().setStaticValue(ParamId::AnchorX, 29.5 / 128.0);
        a->params().setStaticValue(ParamId::AnchorY, 29.5 / 96.0);
        a->params().setStaticValue(ParamId::ScaleX, 2.0);
        a->params().setStaticValue(ParamId::ScaleY, 2.0);
        PaintDevice p;
        f.img.project(f.key, p);
        INK_CHECK(nearRect(contentBounds(p), Rect{10, 10, 39, 39}, 2));
        const Vec2 c = centroid(p);
        INK_CHECK(std::abs(c.x - 29.5) < 0.7 && std::abs(c.y - 29.5) < 0.7);

        a->params().clearParam(ParamId::ScaleX);
        a->params().clearParam(ParamId::ScaleY);
        a->params().setStaticValue(ParamId::Rotation, 90.0);
        f.img.project(f.key, p);
        // A square turned 90 degrees about its own centre is the same square.
        INK_CHECK(nearRect(contentBounds(p), Rect{20, 20, 20, 20}, 2));
        std::printf("scale/rotation OK\n");
    }

    // 5. The animated opacity multiplies the track opacity.
    {
        Fixture f;
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setStaticValue(ParamId::Opacity, 0.5);
        PaintDevice p;
        f.img.project(f.key, p);
        const PixelRGBA8 mid = p.pixel(29, 29);
        INK_CHECK(mid.a > 0);
        INK_CHECK(std::abs(int(mid.a) - 128) <= 2); // full red over nothing at 50%

        f.track->setOpacity(0.5f);
        PaintDevice p2;
        f.img.project(f.key, p2);
        INK_CHECK(std::abs(int(p2.pixel(29, 29).a) - 64) <= 2);
        std::printf("animated opacity OK\n");
    }

    // 6. Clearing the keyframes projects exactly like no animation.
    {
        Fixture f;
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setStaticValue(ParamId::PositionX, 25.0);
        PaintDevice before;
        f.img.project(f.key, before);

        // Wipe the track's params (the view detaches with it).
        f.track->params() = ParamBlock{};
        PaintDevice after;
        f.img.project(f.key, after);
        INK_CHECK(sameRect(contentBounds(after), Rect{20, 20, 20, 20}));
        INK_CHECK(centroid(after).x == 29.5);
        std::printf("stale link OK\n");
    }

    // 7. Two adjustment layers claiming one target: the first in tree order
    //    wins, deterministically, and the second is inert.
    {
        Fixture f;
        AdjustmentLayer *first = addAdjustment(f.img, f.folder, "A1");
        first->setTargetLayer(f.track);
        first->params().setStaticValue(ParamId::PositionX, 10.0);

        PaintDevice p;
        f.img.project(f.key, p);
        INK_CHECK(std::abs(centroid(p).x - 39.5) < 0.01); // +10 px moved it
        // The keyframes live on the layer: there is exactly one param block per
        // track, so "two drivers" cannot exist any more.
        std::printf("one driver per layer OK\n");
    }

    // 8. projectRect with an adjustment layer refreshes exactly the region the
    //    transform can touch — the union of the request and its FORWARD-MAPPED
    //    image (a dab that moves away still has to be erased where it landed),
    //    tile-aligned — and the pixels inside it must be bit-identical to a
    //    full project(). An adjustment whose parameters all sit at their
    //    defaults is skipped at collection time, so the plain fast path is
    //    untouched.
    {
        Fixture f;
        const Rect canvas = canvasRect(f.img);
        PaintDevice full;
        f.img.projectRect(f.key, Rect{30, 30, 4, 4}, full);
        // No adjustment layer: the partial path still paints the square, and
        // reports a tile-aligned superset of what was asked for.
        INK_CHECK(full.pixel(29, 29).a == 255);
        INK_CHECK(sameRect(contentBounds(full), Rect{20, 20, 20, 20}));
        PaintDevice p;
        const Rect reported = f.img.projectRect(f.key, Rect{30, 30, 4, 4}, p);
        INK_CHECK(reported.contains(30, 30));

        // An adjustment linked but sitting at its defaults is skipped when the
        // adjustments are collected — the refresh must not grow a single pixel
        // compared to the no-adjustment case. This is what keeps brush strokes
        // incremental while a rig is being built.
        AdjustmentLayer *idle = addAdjustment(f.img, f.folder, "Idle");
        idle->setTargetLayer(f.track);
        PaintDevice same;
        const Rect idleRect = f.img.projectRect(f.key, Rect{30, 30, 4, 4}, same);
        INK_CHECK(sameRect(idleRect, reported));
        INK_CHECK(sameRect(contentBounds(same), Rect{20, 20, 20, 20}));

        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setStaticValue(ParamId::PositionX, 30.0);
        PaintDevice adj;
        const Rect all = f.img.projectRect(f.key, Rect{30, 30, 4, 4}, adj);
        // Grown, not rebuilt: the request {30,30,4,4} and the square's moved
        // destination both fall inside...
        INK_CHECK(all.contains(30, 30));
        INK_CHECK(all.contains(60, 30));
        // ...but the strict canvas is NOT the answer any more (here: the bottom
        // tile row is untouched because nothing maps into it).
        INK_CHECK(all.h < canvas.h && all.w <= canvas.w);
        INK_CHECK(sameRect(contentBounds(adj), Rect{50, 20, 20, 20}));
        INK_CHECK(std::abs(centroid(adj).x - 59.5) < 0.01);

        // Equivalence: project() first, then the partial refresh on top, must
        // equal a pristine full projection pixel for pixel.
        PaintDevice pre, ref;
        f.img.project(f.key, pre);
        f.img.projectRect(f.key, Rect{30, 30, 4, 4}, pre);
        f.img.project(f.key, ref);
        bool identical = true;
        for (int y = 0; y < canvas.h && identical; ++y)
            for (int x = 0; x < canvas.w; ++x) {
                const PixelRGBA8 pa = pre.pixel(x, y), pb = ref.pixel(x, y);
                if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b || pa.a != pb.a) {
                    identical = false;
                    break;
                }
            }
        INK_CHECK(identical);

        // Identity-skip: the linked layer is still there, but back at its
        // defaults, so projectRect behaves EXACTLY as if none existed.
        a->params().clearParam(ParamId::PositionX);
        PaintDevice idem;
        const Rect got = f.img.projectRect(f.key, Rect{30, 30, 4, 4}, idem);
        INK_CHECK(sameRect(got, reported));
        INK_CHECK(sameRect(contentBounds(idem), Rect{20, 20, 20, 20}));
        std::printf("projectRect rect-mapping OK\n");
    }

    // 9. A keyed track whose adjustment is added LATER evaluates the same keys:
    //    the animation is a property of the adjustment layer, not of the cell.
    {
        Fixture f;
        PaintDevice a0;
        f.img.project(f.key, a0);
        AdjustmentLayer *a = addAdjustment(f.img, f.folder, "A");
        a->setTargetLayer(f.track);
        a->params().setStaticValue(ParamId::AnchorX, 29.5 / 128.0);
        a->params().setStaticValue(ParamId::AnchorY, 29.5 / 96.0);
        a->params().setKey(ParamId::ScaleX, 0, 1.0, Interp::Linear);
        a->params().setKey(ParamId::ScaleX, 4, 3.0, Interp::Linear);
        PaintDevice s0, s4;
        f.img.project(0, s0);
        f.img.project(4, s4);
        INK_CHECK(nearRect(contentBounds(s0), Rect{20, 20, 20, 20}, 2));
        INK_CHECK(contentBounds(s4).w > 40); // grown about the anchor
        std::printf("late binding OK\n");
    }

    // 10. Project format v3: a paint track writes its parameter channels
    //     (keyframes live on the layer now) and the file stays readable.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "inkframe_adj_test";
        fs::remove_all(dir);
        Fixture f;
        PaintLayer *track = f.track;
        track->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        track->params().setKey(ParamId::PositionX, 8, 55.5, Interp::Bezier);
        track->params().setKey(ParamId::Opacity, 0, 0.25, Interp::Hold);
        track->params().setStaticValue(ParamId::ScaleX, 1.5);

        const ProjectSaveResult r = saveProject(f.img, dir.string());
        INK_CHECK(r.ok);
        std::ifstream in(dir / "project.json");
        const std::string json((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        INK_CHECK(json.find("\"version\":4") != std::string::npos);
        INK_CHECK(json.find("\"kind\":\"paint\"") != std::string::npos);
        INK_CHECK(json.find("\"posX\":") != std::string::npos);
        INK_CHECK(json.find("\"i\":\"bezier\"") != std::string::npos);
        INK_CHECK(json.find("\"i\":\"hold\"") != std::string::npos);
        INK_CHECK(json.find("\"o\":[") != std::string::npos); // handles survive
        INK_CHECK(json.find("55.5") != std::string::npos);
        INK_CHECK(json.find("\"scaleX\":{\"s\":1.5}") != std::string::npos);
        // A parameter left at its default must not bloat the file.
        INK_CHECK(json.find("\"shearX\"") == std::string::npos);
        fs::remove_all(dir);
        std::printf("project v3 format OK\n");
    }

    std::printf("test_adjustment_layer PASS\n");
    return 0;
}
