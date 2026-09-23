// Shape rasterizer tests: polyline/rect/ellipse strokes + ellipse fill.
#include "brush/InkShapeRaster.h"
#include "core/InkPaintDevice.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>

using namespace ink;

static int opaquePixels(const PaintDevice &dev, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (dev.pixel(x, y).a > 0)
                ++n;
    return n;
}

int main() {
    // Polyline: endpoints and the middle are stained, off-path pixels are not,
    // and the memento restores the device exactly.
    {
        PaintDevice dev;
        AutoBrushTip tip(BrushTipShape{6.0f, 1.0f, 1.0f, 0.1f});
        TileMemento m;
        Rect r = strokePolyline(dev, {{10.0, 20.0}, {90.0, 20.0}}, tip, {0, 0, 0, 255}, 1.0f,
                                CompositeOp::Normal, &m);
        INK_CHECK(!r.isEmpty());
        INK_CHECK(dev.pixel(10, 20).a > 200);
        INK_CHECK(dev.pixel(50, 20).a > 200);
        INK_CHECK(dev.pixel(90, 20).a > 200);
        INK_CHECK(dev.pixel(50, 60).a == 0);
        INK_CHECK(!m.empty());
        dev.store().restore(m);
        INK_CHECK(dev.pixel(50, 20).a == 0);
        printf("stroke_polyline OK dirty=%d,%d,%d,%d\n", r.x, r.y, r.w, r.h);
    }
    // Spacing walker keeps the line continuous even with wide spacing.
    {
        PaintDevice dev;
        AutoBrushTip tip(BrushTipShape{12.0f, 1.0f, 1.0f, 0.5f}); // 6px spacing
        strokePolyline(dev, {{5.0, 5.0}, {95.0, 5.0}}, tip, {0, 0, 0, 255});
        for (int x = 6; x <= 94; ++x) {
            bool hit = false;
            for (int y = 0; y <= 12 && !hit; ++y)
                hit = dev.pixel(x, y).a > 0;
            INK_CHECK(hit);
        }
        printf("stroke_spacing OK\n");
    }
    // Rectangle: corners and edges stained, interior untouched (stroke mode).
    {
        PaintDevice dev;
        AutoBrushTip tip(BrushTipShape{4.0f, 1.0f, 1.0f, 0.1f});
        strokeRect(dev, 10.0, 10.0, 50.0, 40.0, tip, {0, 0, 0, 255});
        INK_CHECK(dev.pixel(10, 10).a > 200);
        INK_CHECK(dev.pixel(50, 25).a > 200);
        INK_CHECK(dev.pixel(30, 40).a > 200);
        INK_CHECK(dev.pixel(30, 25).a == 0);
        printf("stroke_rect OK\n");
    }
    // Ellipse stroke: extremes stained, center empty.
    {
        PaintDevice dev;
        AutoBrushTip tip(BrushTipShape{4.0f, 1.0f, 1.0f, 0.1f});
        strokeEllipse(dev, 30.5, 30.5, 20.0, 12.0, tip, {0, 0, 0, 255});
        INK_CHECK(dev.pixel(30, 30).a == 0);
        INK_CHECK(dev.pixel(50, 30).a > 200); // right extreme (cx + rx)
        INK_CHECK(dev.pixel(30, 18).a > 200); // top extreme (cy - ry)
        printf("stroke_ellipse OK\n");
    }
    // Ellipse fill: covered area matches pi*rx*ry within 2%.
    {
        PaintDevice dev;
        const double rx = 24.0, ry = 16.0;
        fillEllipse(dev, 40.5, 40.5, rx, ry, {0, 0, 0, 255});
        const int count = opaquePixels(dev, 90, 90);
        const double expect = M_PI * rx * ry;
        INK_CHECK(std::abs(double(count) - expect) / expect < 0.02);
        printf("fill_ellipse OK count=%d expect=%.0f\n", count, expect);
    }
    printf("shapes OK\n");
    return 0;
}
