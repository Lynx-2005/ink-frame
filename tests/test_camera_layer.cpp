// Camera layers: Clip-Studio-style 2D camera for camera control.
//
// Covers the whole feature headlessly (engine only, no Qt):
//   1. identity no-op (absent / invisible / identity all bit-identical)
//   2. creation scoping (layer / folder / global) + auto names + collection
//   3. keyframes (hold/linear/bezier, handles, move/remove, identity)
//   4. layer camera moves one track losslessly (integer pan)
//   5. folder camera moves the whole group as a unit
//   6. global camera reframes the final composite (chained)
//   7. camera opacity multiplies track opacity
//   8. camera tools (pan/zoom/rotate simulated as tool drags write keys)
//   9. combined layer-keys + camera-keys stack (cam * layer)
//   10. smooth / no-glitch (project vs projectRect, monotone bezier, singular,
//       first-wins, missing target inert, perf guard)
//   11. project v4 format (kind camera + scope + params round-trip in JSON)
//   12. placement (layer cam right after target, folder cam first, folder cam
//       dies with its group on ungroup)
#include "core/InkImage.h"
#include "io/InkProject.h"
#include "test_check.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace ink;

namespace {

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
bool nearRect(const Rect &a, const Rect &b, int slack) {
    return std::abs(a.x - b.x) <= slack && std::abs(a.y - b.y) <= slack &&
           std::abs(a.w - b.w) <= 2 * slack && std::abs(a.h - b.h) <= 2 * slack;
}
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
bool samePixels(const PaintDevice &a, const PaintDevice &b, const Rect &r) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const PixelRGBA8 pa = a.pixel(x, y), pb = b.pixel(x, y);
            if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b || pa.a != pb.a) return false;
        }
    return true;
}

} // namespace

int main() {
    // 1. Identity no-op: absent, invisible, and default-valued all match.
    {
        Fixture f;
        PaintDevice ref;
        f.img.project(f.key, ref);
        const Rect full = canvasRect(f.img);
        const auto refSnap = snapshot(ref, full);
        INK_CHECK(sameRect(contentBounds(ref), Rect{20, 20, 20, 20}));

        CameraLayer *cam = f.img.createCamera(f.folder, "Cam", CameraScope::Layer, f.track->id());
        PaintDevice p1;
        f.img.project(f.key, p1);
        INK_CHECK(snapshot(p1, full) == refSnap); // default params = identity

        cam->setVisible(false);
        cam->params().setStaticValue(ParamId::PositionX, 30.0);
        PaintDevice p2;
        f.img.project(f.key, p2);
        INK_CHECK(snapshot(p2, full) == refSnap); // invisible = inert

        cam->setVisible(true);
        cam->params().setStaticValue(ParamId::PositionX, 0.0);
        PaintDevice p3;
        f.img.project(f.key, p3);
        INK_CHECK(snapshot(p3, full) == refSnap);
        std::printf("camera identity no-op OK\n");
    }

    // 2. Creation scoping + auto names + collection.
    {
        Fixture f;
        CameraLayer *c1 = f.img.createCamera(f.folder, {}, CameraScope::Layer, f.track->id());
        CameraLayer *c2 = f.img.createCamera(f.folder, {}, CameraScope::Folder, f.folder->id());
        CameraLayer *c3 = f.img.createCamera(f.folder, {}, CameraScope::Global, 0);
        INK_CHECK(c1 && c2 && c3);
        INK_CHECK(c1->scope() == CameraScope::Layer && c1->targetId() == f.track->id());
        INK_CHECK(c2->scope() == CameraScope::Folder && c2->targetId() == f.folder->id());
        INK_CHECK(c3->scope() == CameraScope::Global && c3->targetId() == 0);
        INK_CHECK(c1->name() != c2->name() && c2->name() != c3->name());
        // Canvas-seeded reference frame keeps the pivot stable.
        INK_CHECK(c1->params().hasFrame());
        INK_CHECK(sameRect(c1->params().frame(), canvasRect(f.img)));
        std::vector<CameraLayer*> all;
        f.img.collectCameras(all);
        INK_CHECK(all.size() == 3);
        INK_CHECK(f.img.cameraById(c2->id()) == c2);
        INK_CHECK(f.img.layerById(c1->id()) == c1);
        INK_CHECK(f.img.folderById(f.folder->id()) == f.folder);
        INK_CHECK(f.img.deleteCamera(c1));
        all.clear();
        f.img.collectCameras(all);
        INK_CHECK(all.size() == 2);
        std::printf("camera scoping OK\n");
    }

    // 3. Keyframes: hold/linear/bezier, handles, move/remove, identity.
    {
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        INK_CHECK(!cam->animated());
        INK_CHECK(cam->params().isIdentityAt(0));
        cam->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        cam->params().setKey(ParamId::PositionX, 10, 40.0, Interp::Bezier);
        INK_CHECK(cam->animated());
        INK_CHECK(!cam->params().isIdentityAt(10));
        INK_CHECK(cam->params().hasKey(ParamId::PositionX, 0));
        // Collinear auto-bezier reads as a straight line (no wobble).
        INK_CHECK(std::abs(cam->params().valueAt(ParamId::PositionX, 5) - 20.0) < 0.6);
        // Hold freezes, linear lerps.
        cam->params().setKey(ParamId::PositionY, 0, 5.0, Interp::Hold);
        cam->params().setKey(ParamId::PositionY, 10, 99.0, Interp::Hold);
        INK_CHECK(std::abs(cam->params().valueAt(ParamId::PositionY, 5) - 5.0) < 1e-6);
        cam->params().setInterp(ParamId::PositionY, 0, Interp::Linear);
        INK_CHECK(std::abs(cam->params().valueAt(ParamId::PositionY, 5) - 52.0) < 1e-6);
        // Handles + move + remove.
        INK_CHECK(cam->params().setHandles(ParamId::PositionX, 0, 0.5, 0.0, 0.5, 0.0));
        INK_CHECK(cam->params().moveKey(ParamId::PositionX, 10, 12));
        INK_CHECK(!cam->params().hasKey(ParamId::PositionX, 10));
        INK_CHECK(cam->params().hasKey(ParamId::PositionX, 12));
        INK_CHECK(cam->params().removeKey(ParamId::PositionX, 12));
        INK_CHECK(!cam->params().hasKey(ParamId::PositionX, 12));
        INK_CHECK(cam->params().removeKeysAt(0) > 0);
        std::printf("camera keyframes OK\n");
    }

    // 4. Layer camera pans one track losslessly (integer move = bit-exact).
    {
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        cam->params().setStaticValue(ParamId::PositionX, 16.0);
        cam->params().setStaticValue(ParamId::PositionY, 8.0);
        PaintDevice moved;
        f.img.project(f.key, moved);
        INK_CHECK(sameRect(contentBounds(moved), Rect{36, 28, 20, 20}));
        const Vec2 c = centroid(moved);
        INK_CHECK(std::abs(c.x - 45.5) < 0.01 && std::abs(c.y - 37.5) < 0.01);
        Fixture ref;
        PaintDevice base;
        ref.img.project(ref.key, base);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const PixelRGBA8 s = base.pixel(20 + x, 20 + y);
                const PixelRGBA8 d = moved.pixel(36 + x, 28 + y);
                INK_CHECK(s.r == d.r && s.g == d.g && s.b == d.b && s.a == d.a);
            }
        std::printf("camera layer pan lossless OK\n");
    }

    // 5. Folder camera moves the group as a unit; layer camera moves one track.
    {
        Image img{128, 96, 12};
        GroupLayer *g = img.createFolder("G");
        PaintLayer *t1 = img.createTrack(g, "T1");
        PaintLayer *t2 = img.createTrack(g, "T2");
        t1->deviceAt(0, true)->fillRect(Rect{10, 10, 10, 10}, PixelRGBA8{200, 40, 40, 255},
                                        CompositeOp::Normal);
        t2->deviceAt(0, true)->fillRect(Rect{60, 60, 10, 10}, PixelRGBA8{40, 200, 40, 255},
                                        CompositeOp::Normal);
        t1->setSpanEnd(0, kOpenEnd);
        t2->setSpanEnd(0, kOpenEnd);
        CameraLayer *fc = img.createCamera(g, "FC", CameraScope::Folder, g->id());
        fc->params().setStaticValue(ParamId::PositionX, 10.0);
        PaintDevice p;
        img.project(0, p);
        // Both squares shifted +10 in x.
        INK_CHECK(p.pixel(20, 15).a == 255 && p.pixel(70, 65).a == 255);
        INK_CHECK(p.pixel(10, 15).a == 0 && p.pixel(60, 65).a == 0);
        // Layer camera on t1 only: t2 stays where the folder camera put it.
        CameraLayer *lc = img.createCamera(g, "LC", CameraScope::Layer, t1->id());
        lc->params().setStaticValue(ParamId::PositionX, 5.0);
        PaintDevice p2;
        img.project(0, p2);
        INK_CHECK(p2.pixel(25, 15).a == 255); // t1: folder 10 + layer 5
        INK_CHECK(p2.pixel(70, 65).a == 255); // t2: folder 10 only
        std::printf("camera folder scope OK\n");
    }

    // 6. Global camera reframes the final composite (chained in tree order).
    {
        Fixture f;
        CameraLayer *gc = f.img.createCamera(f.folder, "G", CameraScope::Global, 0);
        gc->params().setStaticValue(ParamId::PositionX, 10.0);
        PaintDevice p;
        f.img.project(0, p);
        INK_CHECK(sameRect(contentBounds(p), Rect{30, 20, 20, 20}));
        CameraLayer *gc2 = f.img.createCamera(f.folder, "G2", CameraScope::Global, 0);
        gc2->params().setStaticValue(ParamId::PositionY, 5.0);
        PaintDevice p2;
        f.img.project(0, p2);
        INK_CHECK(sameRect(contentBounds(p2), Rect{30, 25, 20, 20}));
        std::printf("camera global OK\n");
    }

    // 7. Camera opacity multiplies track opacity.
    {
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        cam->params().setStaticValue(ParamId::Opacity, 0.5);
        PaintDevice p;
        f.img.project(0, p);
        INK_CHECK(std::abs(int(p.pixel(29, 29).a) - 128) <= 2);
        f.track->setOpacity(0.5f);
        PaintDevice p2;
        f.img.project(0, p2);
        INK_CHECK(std::abs(int(p2.pixel(29, 29).a) - 64) <= 2);
        std::printf("camera opacity OK\n");
    }

    // 8. Camera tools: simulated drags (pan/zoom/rotate) write smooth keys.
    {
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        // Tool drag: key a pan across 3 frames (collinear => straight line).
        cam->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        cam->params().setKey(ParamId::PositionX, 1, 5.0, Interp::Bezier);
        cam->params().setKey(ParamId::PositionX, 2, 10.0, Interp::Bezier);
        cam->params().refreshAllAutoHandles();
        INK_CHECK(std::abs(cam->params().valueAt(ParamId::PositionX, 0.5) - 2.5) < 0.6);
        INK_CHECK(std::abs(cam->params().valueAt(ParamId::PositionX, 1.5) - 7.5) < 0.6);
        // Zoom tool: uniform scale doubles about canvas centre.
        cam->params().setStaticValue(ParamId::ScaleX, 2.0);
        cam->params().setStaticValue(ParamId::ScaleY, 2.0);
        PaintDevice z;
        f.img.project(0, z);
        INK_CHECK(!contentBounds(z).isEmpty());
        cam->params().clearParam(ParamId::ScaleX);
        cam->params().clearParam(ParamId::ScaleY);
        // Rotate tool: 360deg == identity (smooth wrap, no glitch).
        cam->params().setStaticValue(ParamId::Rotation, 360.0);
        PaintDevice r1;
        f.img.project(0, r1);
        cam->params().setStaticValue(ParamId::Rotation, 0.0);
        PaintDevice r0;
        f.img.project(0, r0);
        INK_CHECK(samePixels(r0, r1, canvasRect(f.img)));
        std::printf("camera tools OK\n");
    }

    // 9. Combined: layer keys + camera keys stack as cam * layer.
    {
        Fixture f;
        f.track->params().setStaticValue(ParamId::PositionX, 10.0);
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        cam->params().setStaticValue(ParamId::PositionX, 7.0);
        PaintDevice p;
        f.img.project(0, p);
        // Total +17 in x.
        INK_CHECK(sameRect(contentBounds(p), Rect{37, 20, 20, 20}));
        INK_CHECK(std::abs(centroid(p).x - 46.5) < 0.6);
        std::printf("camera combined OK\n");
    }

    // 10. Smooth / no-glitch: incremental == full, monotone, singular,
    //     first-wins, missing target inert, perf guard.
    {
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "C", CameraScope::Layer, f.track->id());
        cam->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        cam->params().setKey(ParamId::PositionX, 10, 20.0, Interp::Bezier);
        // MonotoneBezier: values never leave [0,20] on a collinear run.
        for (int i = 0; i <= 10; ++i) {
            const double v = cam->params().valueAt(ParamId::PositionX, i);
            INK_CHECK(v >= -1e-6 && v <= 20.0 + 1e-6);
        }
        // projectRect with an active camera falls back to full (correctness
        // first) — pixels still equal a pristine full projection.
        PaintDevice full;
        f.img.project(5, full);
        PaintDevice part;
        const Rect got = f.img.projectRect(5, Rect{30, 30, 4, 4}, part);
        INK_CHECK(!got.isEmpty());
        INK_CHECK(samePixels(full, part, canvasRect(f.img)));
        // Singular scale 0 never crashes and empties (or holds) gracefully.
        cam->params().clearParam(ParamId::PositionX);
        cam->params().setStaticValue(ParamId::ScaleX, 0.0);
        cam->params().setStaticValue(ParamId::ScaleY, 0.0);
        PaintDevice sing;
        f.img.project(5, sing); // must not crash
        PaintDevice singRect;
        f.img.projectRect(5, Rect{0, 0, 32, 32}, singRect);
        INK_CHECK(samePixels(sing, singRect, canvasRect(f.img)));
        cam->params().clearParam(ParamId::ScaleX);
        cam->params().clearParam(ParamId::ScaleY);
        // First in tree order wins for one target.
        cam->params().setStaticValue(ParamId::PositionX, 5.0);
        CameraLayer *second =
            f.img.createCamera(f.folder, "C2", CameraScope::Layer, f.track->id());
        second->params().setStaticValue(ParamId::PositionX, 50.0);
        PaintDevice w;
        f.img.project(0, w);
        INK_CHECK(std::abs(centroid(w).x - 34.5) < 0.7); // +5 won, not +50
        // Missing target is inert.
        second->setTargetId(999999);
        PaintDevice w2;
        f.img.project(0, w2);
        INK_CHECK(std::abs(centroid(w2).x - 34.5) < 0.7);
        // Perf: full-canvas camera projection stays interactive.
        auto t0 = std::chrono::steady_clock::now();
        PaintDevice perf;
        for (int i = 0; i < 5; ++i) f.img.project(i % 11, perf);
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        INK_CHECK(ms < 2000);
        std::printf("camera smooth OK (%lldms)\n", (long long)ms);
    }

    // 11. Project v4: kind camera + scope + params survive in JSON.
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "inkframe_cam_test";
        fs::remove_all(dir);
        Fixture f;
        CameraLayer *cam = f.img.createCamera(f.folder, "Shot", CameraScope::Folder, f.folder->id());
        cam->params().setKey(ParamId::PositionX, 0, 0.0, Interp::Bezier);
        cam->params().setKey(ParamId::PositionX, 8, 55.5, Interp::Bezier);
        cam->params().setKey(ParamId::Opacity, 0, 0.25, Interp::Hold);
        const ProjectSaveResult r = saveProject(f.img, dir.string());
        INK_CHECK(r.ok);
        std::ifstream in(dir / "project.json");
        const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        INK_CHECK(json.find("\"version\":4") != std::string::npos);
        INK_CHECK(json.find("\"kind\":\"camera\"") != std::string::npos);
        INK_CHECK(json.find("\"scope\":\"folder\"") != std::string::npos);
        INK_CHECK(json.find("\"posX\":") != std::string::npos);
        INK_CHECK(json.find("\"i\":\"bezier\"") != std::string::npos);
        INK_CHECK(json.find("\"i\":\"hold\"") != std::string::npos);
        INK_CHECK(json.find("55.5") != std::string::npos);
        // Paint entries stay v3-compatible.
        INK_CHECK(json.find("\"kind\":\"paint\"") != std::string::npos);
        fs::remove_all(dir);
        std::printf("camera project v4 OK\n");
    }

    // 12. Placement: layer cameras sit right after their target, folder
    //     cameras sit first in the folder, and a folder camera dies with its
    //     group on ungroup (it cannot frame a dissolved folder).
    {
        Image img{128, 96, 12};
        GroupLayer *g = img.createFolder("G");
        PaintLayer *t1 = img.createTrack(g, "T1");
        PaintLayer *t2 = img.createTrack(g, "T2");
        // Layer camera for T1 lands directly after T1, not at the bottom.
        CameraLayer *lc = img.createCamera(g, "LC", CameraScope::Layer, t1->id(), 1);
        INK_CHECK(g->children().size() == 3);
        INK_CHECK(g->children()[0].get() == static_cast<Layer*>(t1));
        INK_CHECK(g->children()[1].get() == static_cast<Layer*>(lc));
        INK_CHECK(g->children()[2].get() == static_cast<Layer*>(t2));
        // Default placement still appends (existing callers unaffected).
        CameraLayer *tail = img.createCamera(g, "Tail", CameraScope::Global, 0);
        INK_CHECK(g->children().back().get() == static_cast<Layer*>(tail));
        img.deleteCamera(tail);
        // Folder camera goes first ...
        CameraLayer *fc = img.createCamera(g, "FC", CameraScope::Folder, g->id(), 0);
        INK_CHECK(g->children().front().get() == static_cast<Layer*>(fc));
        // ... and still reframes the whole group as a unit from there.
        t1->deviceAt(0, true)->fillRect(Rect{10, 10, 10, 10}, PixelRGBA8{200, 40, 40, 255},
                                        CompositeOp::Normal);
        t2->deviceAt(0, true)->fillRect(Rect{60, 60, 10, 10}, PixelRGBA8{40, 200, 40, 255},
                                        CompositeOp::Normal);
        t1->setSpanEnd(0, kOpenEnd);
        t2->setSpanEnd(0, kOpenEnd);
        fc->params().setStaticValue(ParamId::PositionX, 10.0);
        PaintDevice p;
        img.project(0, p);
        INK_CHECK(p.pixel(20, 15).a == 255 && p.pixel(70, 65).a == 255);
        INK_CHECK(p.pixel(10, 15).a == 0 && p.pixel(60, 65).a == 0);
        // Ungroup dissolves the group: paints move on, the folder camera is
        // dropped, layer cameras survive with their (moved) targets.
        GroupLayer *g2 = img.createFolder("G2");
        img.createTrack(g2, "T3");
        INK_CHECK(img.ungroupFolder(g));
        std::vector<CameraLayer*> all;
        img.collectCameras(all);
        bool fcAlive = false, lcAlive = false;
        for (auto *c : all) {
            if (c == fc) fcAlive = true;
            if (c == lc) lcAlive = true;
        }
        INK_CHECK(!fcAlive && lcAlive);
        std::printf("camera placement OK\n");
    }

    std::printf("test_camera_layer PASS\n");
    return 0;
}
