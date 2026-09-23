// Floating regions (move tool machinery): lift, erase, stamp, flip, and the
// mask translate/mirror used while dragging.
#include "core/InkPaintDevice.h"
#include "core/InkRegion.h"
#include "core/InkSelection.h"
#include "test_check.h"
#include <cstdio>

using namespace ink;

int main() {
    // Lift keeps only masked pixels, erase clears them, stamp lands them at an
    // offset, and one memento undoes the whole thing.
    {
        PaintDevice dev;
        dev.fillRect({10, 10, 20, 10}, {255, 0, 0, 255});
        Selection sel;
        sel.reset(64, 64);
        sel.applyPolygon({{10, 10}, {30, 10}, {30, 20}, {10, 20}}, SelectionAction::Replace, false);

        const FloatRegion lifted = liftRegion(dev, sel.bounds(), &sel);
        INK_CHECK(!lifted.empty());
        INK_CHECK(lifted.bounds.w == 20 && lifted.bounds.h == 10);
        INK_CHECK(lifted.px[0].r == 255 && lifted.px[0].a == 255);

        TileMemento m;
        INK_CHECK(!eraseRegion(dev, sel.bounds(), &sel, &m).isEmpty());
        INK_CHECK(dev.pixel(15, 15).a == 0);
        const Rect stamped = stampRegion(dev, lifted, 25, 40, {0, 0, 64, 64}, &m);
        INK_CHECK(!stamped.isEmpty());
        INK_CHECK(dev.pixel(40, 55).a == 255); // moved pixel 15+25, 15+40
        INK_CHECK(dev.pixel(15, 15).a == 0);   // source stays empty
        dev.store().restore(m);
        INK_CHECK(dev.pixel(15, 15).a == 255 && dev.pixel(40, 55).a == 0);
        printf("region lift/erase/stamp/undo OK\n");
    }
    // Stamping is clipped to the document rect: partial overlap lands only the
    // inside part, a fully outside stamp does nothing.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 10, 10}, {0, 255, 0, 255});
        Selection sel;
        sel.reset(64, 64);
        sel.applyPolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, SelectionAction::Replace, false);
        const FloatRegion r = liftRegion(dev, sel.bounds(), &sel);
        const Rect partial = stampRegion(dev, r, 60, 0, {0, 0, 64, 64}, nullptr);
        INK_CHECK(partial.x == 60 && partial.w == 4 && partial.h == 10);
        INK_CHECK(dev.pixel(62, 5).a == 255);
        INK_CHECK(dev.pixel(5, 5).a == 255); // source untouched (no erase here)
        INK_CHECK(stampRegion(dev, r, 100, 0, {0, 0, 64, 64}, nullptr).isEmpty());
        printf("region document clip OK\n");
    }
    // Flip mirrors the lifted buffer.
    {
        PaintDevice dev;
        dev.fillRect({0, 0, 4, 1}, {10, 20, 30, 255});
        dev.fillRect({0, 0, 1, 1}, {200, 0, 0, 255}); // leftmost pixel is red
        const FloatRegion r = liftRegion(dev, {0, 0, 4, 1}, nullptr);
        const FloatRegion h = flipRegion(r, true);
        INK_CHECK(h.px[0].r == 10 && h.px[3].r == 200);
        const FloatRegion v = flipRegion(r, false);
        INK_CHECK(v.px[0].r == r.px[0].r); // one row: vertical flip is identity
        printf("region flip OK\n");
    }
    // Mask translate keeps coverage and bounds consistent.
    {
        Selection sel;
        sel.reset(64, 64);
        sel.applyPolygon({{10, 10}, {20, 10}, {20, 20}, {10, 20}}, SelectionAction::Replace, false);
        sel.translate(5, 7);
        INK_CHECK(sel.contains(20, 20));
        INK_CHECK(!sel.contains(15, 15));
        INK_CHECK(sel.bounds().x == 15 && sel.bounds().y == 17);
        INK_CHECK(sel.bounds().w == 10 && sel.bounds().h == 10);
        printf("selection translate OK\n");
    }
    // Mask mirror flips inside its own bounds (checked on an L-shape, since a
    // shape that fills its bbox mirrors onto itself).
    {
        Selection sel;
        sel.reset(64, 64);
        sel.applyPolygon({{10, 10}, {14, 10}, {14, 12}, {10, 12}}, SelectionAction::Replace, false);
        sel.applyPolygon({{10, 12}, {11, 12}, {11, 14}, {10, 14}}, SelectionAction::Add, false);
        INK_CHECK(sel.contains(10, 13));  // leg on the left
        INK_CHECK(!sel.contains(13, 13));
        sel.mirror(true);
        INK_CHECK(!sel.contains(10, 13));
        INK_CHECK(sel.contains(13, 13));  // leg moved to the right
        printf("selection mirror OK\n");
    }
    printf("region OK\n");
    return 0;
}
