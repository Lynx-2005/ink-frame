// Cell exposure (headless): a cell owns an explicit span, so resizing or
// moving one cell never resizes or moves another. Rendering obeys the spans —
// a gap between cells exposes nothing.
#include "core/InkImage.h"
#include "core/InkLayer.h"
#include <iostream>

#define INK_CHECK(cond) do { if (!(cond)) { \
    std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; return 1; } } while (0)

using namespace ink;

int main() {
    // 1. spans: independent resize, never overlapping the neighbour
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);  // a fresh key holds by default
        l.setKeyAt(10, true);
        INK_CHECK(l.spanEnd(0) == 9);          // clipped by the next key
        INK_CHECK(l.spanEnd(10) == kOpenEnd);  // open to the end
        l.setSpanEnd(0, 4);
        INK_CHECK(l.spanEnd(0) == 4);
        INK_CHECK(l.spanEnd(10) == kOpenEnd);  // untouched
        l.setSpanEnd(10, 20);
        INK_CHECK(l.spanEnd(0) == 4 && l.spanEnd(10) == 20);
        l.setSpanEnd(0, 100);                  // can't swallow the neighbour
        INK_CHECK(l.spanEnd(0) == 9);
        INK_CHECK(l.spanEnd(10) == 20);
        l.setSpanEnd(0, 0);                    // a cell is at least one frame
        INK_CHECK(l.spanEnd(0) == 0);
        INK_CHECK(l.spanEndClamped(10, 30) == 20);
        INK_CHECK(l.spanEndClamped(0, 30) == 0);
    }
    // 2. exposureAt: gaps expose nothing; holds expose the same cel
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);
        l.setKeyAt(5, true);
        l.setKeyAt(9, true);
        l.setSpanEnd(0, 2);   // exposed 0..2, gap at 3..4
        l.setSpanEnd(5, 5);   // exposed 5..5, gap at 6..8
                          // key 9 stays open
        INK_CHECK(l.exposureAt(0) == 0);
        INK_CHECK(l.exposureAt(2) == 0);
        INK_CHECK(l.exposureAt(3) == -1);
        INK_CHECK(l.exposureAt(4) == -1);
        INK_CHECK(l.exposureAt(5) == 5);
        INK_CHECK(l.exposureAt(7) == -1);
        INK_CHECK(l.exposureAt(9) == 9);
        INK_CHECK(l.exposureAt(500) == 9); // open span
        INK_CHECK(l.exposureAt(-1) == -1);
    }
    // 3. splitting a longer cell trims the earlier exposure
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);
        l.setSpanEnd(0, 20);
        l.setKeyAt(10, true);
        INK_CHECK(l.spanEnd(0) == 9);
        INK_CHECK(l.exposureAt(9) == 0 && l.exposureAt(10) == 10);
    }
    // 4. moving / copying a cell carries its exposure length
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);
        l.setSpanEnd(0, 3);   // 4 frames
        INK_CHECK(l.moveDeviceContent(0, 20, false));
        INK_CHECK(l.spanEnd(20) == 23);
        l.copyDeviceContent(20, 30);
        INK_CHECK(l.spanEnd(30) == 33);
    }
    // 5. projection honours gaps: no exposure, no pixels
    {
        Image img(32, 32, 12);
        GroupLayer *f = img.ensureFolder();
        PaintLayer *l = img.createTrack(f, "T");
        l->setKeyAt(0, true);
        l->setSpanEnd(0, 0);            // 1-frame cell
        l->deviceAt(0, false)->setPixel(4, 4, {255, 0, 0, 255});
        img.setFrameCount(10);

        PaintDevice out;
        img.project(0, out);
        INK_CHECK(!out.extent().isEmpty());
        img.project(1, out);            // gap -> nothing exposed
        INK_CHECK(out.extent().isEmpty());
        // extending the same cell exposes it there too
        l->setSpanEnd(0, 4);
        img.project(4, out);
        INK_CHECK(!out.extent().isEmpty());
        img.project(5, out);
        INK_CHECK(out.extent().isEmpty());
    }
    // 6. shifting keys keeps their exposure lengths
    {
        PaintLayer l("L");
        l.setKeyAt(0, true);
        l.setSpanEnd(0, 0);
        l.setKeyAt(4, true);
        l.setSpanEnd(4, 4);
        INK_CHECK(l.shiftFrom(4, 2, 100) == 1);
        INK_CHECK(l.hasKeyAt(6) && !l.hasKeyAt(4));
        INK_CHECK(l.spanEnd(6) == 6);   // length preserved
        INK_CHECK(l.spanEnd(0) == 0);   // untouched
    }
    std::cout << "test_cells OK\n";
    return 0;
}
