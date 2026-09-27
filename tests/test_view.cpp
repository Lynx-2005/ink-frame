// ViewConverter: round-trip, fit, still-point zoom, DPR independence,
// plus dab-lands-on-cursor proof (stroke at mapped point paints that pixel).
#include "brush/InkPaintOp.h"
#include "core/InkStroke.h"
#include "core/InkViewConverter.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>

using namespace ink;
static bool near(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

int main() {
    // Round-trip at various zooms / rotations / pans / DPRs
    {
        ViewConverter v;
        v.setDocSize(1920, 1080);
        v.setWidgetSize(800, 600);
        for (double dpr : {1.0, 1.25, 1.5, 2.0}) {
            v.setDevicePixelRatio(dpr);
            for (double z : {0.05, 0.5, 1.0, 4.0}) {
                v.fit();
                v.zoomAtCenter(z);
                v.setRotation(15.0);
                v.panBy({37, -53});
                for (Vec2 p : {Vec2{0, 0}, Vec2{960, 540}, Vec2{1919, 1079}, Vec2{100, 900}}) {
                    Vec2 w = v.mapToWidget(p);
                    Vec2 back = v.mapToDoc(w);
                    INK_CHECK(near(back.x, p.x, 1e-6) && near(back.y, p.y, 1e-6));
                }
                // DPR must not affect logical mapping
                Vec2 w1 = v.mapToWidget({500, 500});
                v.setDevicePixelRatio(3.0);
                Vec2 w2 = v.mapToWidget({500, 500});
                INK_CHECK(near(w1.x, w2.x) && near(w1.y, w2.y));
                v.setDevicePixelRatio(dpr);
            }
        }
        printf("roundtrip+dpr OK\n");
    }
    // Fit centers the document
    {
        ViewConverter v;
        v.setDocSize(1920, 1080);
        v.setWidgetSize(800, 600);
        v.fit();
        Vec2 c = v.mapToWidget({960, 540});
        INK_CHECK(near(c.x, 400.0, 1e-6) && near(c.y, 300.0, 1e-6));
        INK_CHECK(v.zoom() > 0.3 && v.zoom() < 0.5); // min(800/1920,600/1080)*0.95
        printf("fit OK zoom=%f\n", v.zoom());
    }
    // Fit keeps a ROTATED canvas inside the widget: every doc corner maps
    // within the widget rect at any angle.
    {
        ViewConverter v;
        v.setDocSize(1920, 1080);
        v.setWidgetSize(800, 600);
        for (double angle : {0.0, 15.0, 30.0, 45.0, 90.0, 137.0}) {
            v.setRotation(angle);
            v.fit();
            for (int i = 0; i < 4; ++i) {
                Vec2 corner{i & 1 ? 1920.0 : 0.0, i & 2 ? 1080.0 : 0.0};
                Vec2 w = v.mapToWidget(corner);
                INK_CHECK(w.x >= 0.0 && w.x <= 800.0);
                INK_CHECK(w.y >= 0.0 && w.y <= 600.0);
            }
            const Vec2 c = v.mapToWidget({960, 540});
            INK_CHECK(near(c.x, 400.0, 1e-6) && near(c.y, 300.0, 1e-6));
        }
        v.setRotation(30.0);
        v.fit();
        INK_CHECK(v.zoom() < 800.0 / 1920.0 * 0.95); // must zoom out to clear the corners
        printf("rotated_fit OK zoom=%f\n", v.zoom());
    }
    // Still-point zoom: doc point under cursor is identical before/after
    {
        ViewConverter v;
        v.setDocSize(1920, 1080);
        v.setWidgetSize(800, 600);
        v.fit();
        v.setRotation(30.0);
        Vec2 cursor{123, 456};
        Vec2 before = v.mapToDoc(cursor);
        v.zoomAt(cursor, 2.5);
        Vec2 after = v.mapToDoc(cursor);
        INK_CHECK(near(before.x, after.x, 1e-6) && near(before.y, after.y, 1e-6));
        v.zoomAt(cursor, 0.1);
        Vec2 after2 = v.mapToDoc(cursor);
        INK_CHECK(near(before.x, after2.x, 1e-6) && near(before.y, after2.y, 1e-6));
        printf("stillpoint OK\n");
    }
    // Dab lands on cursor: map widget->doc, stamp, project pixel is painted
    {
        ViewConverter v;
        v.setDocSize(200, 200);
        v.setWidgetSize(800, 600);
        v.fit();
        Vec2 cursor{400, 300}; // widget center == doc center
        Vec2 doc = v.mapToDoc(cursor);
        PaintDevice dev;
        PixelBrushOp op(AutoBrushTip(BrushTipShape{12.0f, 0.9f, 1.0f, 0.05f}), BrushDynamics{});
        TileMemento m;
        op.beginStroke({&dev, {255, 0, 0, 255}, CompositeOp::Normal, &m});
        StrokeEvent e;
        e.state.x = float(doc.x);
        e.state.y = float(doc.y);
        e.state.pressure = 1.0f;
        op.addEvent(e);
        op.endStroke();
        PixelRGBA8 px = dev.pixel(int(std::round(doc.x)), int(std::round(doc.y)));
        INK_CHECK(px.r > 200 && px.a > 200);
        printf("dab-on-cursor OK doc=(%f,%f)\n", doc.x, doc.y);
    }
    // Undo removes exactly one stroke; redo restores it (after-tiles path)
    {
        PaintDevice d;
        TileMemento m;
        d.fillRect({0, 0, 8, 8}, {9, 9, 9, 255}, CompositeOp::Normal, &m);
        StrokeCommand cmd;
        cmd.name = "s";
        cmd.targets.push_back({&d, TileMemento{}});
        cmd.targets[0].memento = std::move(m);
        for (auto &e : cmd.targets[0].memento.entries)
            cmd.after.push_back({&d, e.pos, d.store().tile(e.pos.tx, e.pos.ty)});
        UndoStack u;
        u.push(std::move(cmd));
        u.undo();
        INK_CHECK(d.pixel(0, 0).a == 0);
        u.redo();
        PixelRGBA8 p = d.pixel(0, 0);
        INK_CHECK(p.r == 9 && p.a == 255);
        printf("undo_redo_aftertiles OK\n");
    }
    printf("test_view PASS\n");
    return 0;
}
