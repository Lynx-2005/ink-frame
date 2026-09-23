// Flood fill tests: contiguity, tolerance, similar-pixels mode, selection
// clipping and undo capture.
#include "core/InkFloodFill.h"
#include "core/InkPaintDevice.h"
#include "core/InkSelection.h"
#include "test_check.h"
#include <cstdio>

using namespace ink;

namespace {

int countColor(const PaintDevice &dev, int w, int h, PixelRGBA8 c) {
    int n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const PixelRGBA8 p = dev.pixel(x, y);
            if (p.r == c.r && p.g == c.g && p.b == c.b && p.a == c.a)
                ++n;
        }
    return n;
}

} // namespace

int main() {
    const PixelRGBA8 red{255, 0, 0, 255};
    const PixelRGBA8 white{255, 255, 255, 255};
    const PixelRGBA8 green{0, 200, 40, 255};

    // Contiguous fill stays inside the enclosed region.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 64, 64}, red);
        dev.fillRect({16, 16, 32, 32}, white);
        FloodFillParams p;
        Rect dirty = floodFill(dev, dev, {0, 0, 64, 64}, 32, 32, green, p, nullptr);
        INK_CHECK(!dirty.isEmpty());
        INK_CHECK(dev.pixel(20, 20).r == green.r && dev.pixel(20, 20).a == 255);
        INK_CHECK(dev.pixel(5, 5).r == 255 && dev.pixel(5, 5).g == 0);
        INK_CHECK(countColor(dev, 64, 64, green) == 32 * 32);
        printf("fill_contiguous OK dirty=%d,%d,%d,%d\n", dirty.x, dirty.y, dirty.w, dirty.h);
    }
    // Filling with the seed color is a no-op.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 32, 32}, red);
        FloodFillParams p;
        Rect dirty = floodFill(dev, dev, {0, 0, 32, 32}, 5, 5, red, p, nullptr);
        INK_CHECK(dirty.isEmpty());
        printf("fill_noop OK\n");
    }
    // Non-contiguous mode fills every similar pixel, separated or not.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 64, 64}, red);
        dev.fillRect({8, 8, 16, 16}, white);
        dev.fillRect({40, 40, 16, 16}, white);
        FloodFillParams p;
        p.contiguous = false;
        floodFill(dev, dev, {0, 0, 64, 64}, 12, 12, green, p, nullptr);
        INK_CHECK(dev.pixel(12, 12).r == green.r);
        INK_CHECK(dev.pixel(44, 44).r == green.r);
        INK_CHECK(dev.pixel(2, 2).r == red.r);
        printf("fill_similar OK\n");
    }
    // Tolerance widens what counts as the same region.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 64, 64}, red);
        dev.fillRect({20, 20, 24, 24}, {255, 10, 10, 255});
        FloodFillParams tight;
        tight.tolerance = 0;
        floodFill(dev, dev, {0, 0, 64, 64}, 30, 30, green, tight, nullptr);
        INK_CHECK(dev.pixel(2, 2).r == 255 && dev.pixel(2, 2).g == 0); // outer red untouched
        INK_CHECK(dev.pixel(30, 30).r == green.r);
    }
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 64, 64}, red);
        dev.fillRect({20, 20, 24, 24}, {255, 10, 10, 255});
        FloodFillParams loose;
        loose.tolerance = 40;
        floodFill(dev, dev, {0, 0, 64, 64}, 30, 30, green, loose, nullptr);
        INK_CHECK(dev.pixel(2, 2).r == green.r); // near-red joined the region
        printf("fill_tolerance OK\n");
    }
    // Selection clips the fill and the memento restores it exactly.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 64, 64}, red);
        dev.fillRect({16, 16, 32, 32}, white);
        Selection sel;
        sel.reset(64, 64);
        sel.applyPolygon({{16, 16}, {32, 16}, {32, 32}, {16, 32}}, SelectionAction::Replace, false);
        FloodFillParams p;
        p.clip = &sel;
        TileMemento m;
        floodFill(dev, dev, {0, 0, 64, 64}, 20, 20, green, p, &m);
        INK_CHECK(dev.pixel(20, 20).r == green.r);
        INK_CHECK(dev.pixel(40, 20).r == 255 && dev.pixel(40, 20).g == 255); // right half white
        INK_CHECK(!m.empty());
        dev.store().restore(m);
        INK_CHECK(dev.pixel(20, 20).r == 255 && dev.pixel(20, 20).g == 255);
        printf("fill_clip_undo OK\n");
    }
    printf("fill OK\n");
    return 0;
}
