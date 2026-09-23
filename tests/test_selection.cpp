// Selection mask tests: polygon rasterization, boolean ops, outline, clipping.
#include "core/InkPaintDevice.h"
#include "core/InkSelection.h"
#include "test_check.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ink;

int main() {
    // Rectangle polygon: interior selected, exterior not, bounds exact.
    {
        Selection s;
        s.reset(100, 80);
        INK_CHECK(s.isEmpty());
        s.applyPolygon({{10, 10}, {50, 10}, {50, 40}, {10, 40}}, SelectionAction::Replace, true);
        INK_CHECK(!s.isEmpty());
        INK_CHECK(s.contains(30, 25));
        INK_CHECK(!s.contains(5, 5));
        INK_CHECK(s.coverage(25, 20) == 255);
        INK_CHECK(s.bounds().x == 10 && s.bounds().y == 10 && s.bounds().w == 40 &&
                  s.bounds().h == 30);
        printf("select_rect OK bounds=%d,%d,%d,%d\n", s.bounds().x, s.bounds().y, s.bounds().w,
               s.bounds().h);
    }
    // Add / subtract / intersect.
    {
        Selection s;
        s.reset(100, 80);
        s.applyPolygon({{10, 10}, {50, 10}, {50, 40}, {10, 40}}, SelectionAction::Replace, false);
        s.applyPolygon({{40, 20}, {80, 20}, {80, 50}, {40, 50}}, SelectionAction::Add, false);
        INK_CHECK(s.contains(20, 25) && s.contains(60, 45));
        s.applyPolygon({{0, 0}, {100, 0}, {100, 30}, {0, 30}}, SelectionAction::Subtract, false);
        INK_CHECK(!s.contains(20, 25));
        INK_CHECK(s.contains(60, 45));

        Selection t;
        t.reset(100, 80);
        t.applyPolygon({{0, 0}, {30, 0}, {30, 80}, {0, 80}}, SelectionAction::Replace, false);
        t.applyPolygon({{15, 0}, {100, 0}, {100, 80}, {15, 80}}, SelectionAction::Intersect, false);
        INK_CHECK(!t.isEmpty());
        INK_CHECK(t.contains(20, 40) && !t.contains(5, 40) && !t.contains(50, 40));
        printf("select_ops OK\n");
    }
    // A rectangle's outline is exactly four runs.
    {
        Selection s;
        s.reset(60, 60);
        s.applyPolygon({{10, 10}, {40, 10}, {40, 30}, {10, 30}}, SelectionAction::Replace, false);
        INK_CHECK(s.outline().size() == 4);
        printf("select_outline OK segments=%zu\n", s.outline().size());
    }
    // Select all / invert / clear.
    {
        Selection s;
        s.reset(20, 20);
        s.selectAll();
        INK_CHECK(s.contains(0, 0) && s.contains(19, 19));
        s.invert();
        INK_CHECK(s.isEmpty());
        s.applyPolygon({{5, 5}, {10, 5}, {10, 10}, {5, 10}}, SelectionAction::Replace, false);
        s.invert();
        INK_CHECK(s.contains(0, 0) && !s.contains(7, 7));
        s.clear();
        INK_CHECK(s.isEmpty());
        printf("select_all_invert OK\n");
    }
    // Non-zero winding: a doubly wound loop still selects (an even-odd fill
    // would cancel it out), which is what a self-crossing lasso needs.
    {
        Selection s;
        s.reset(80, 60);
        s.applyPolygon({{10, 10}, {50, 10}, {50, 40}, {10, 40},
                        {10, 10}, {50, 10}, {50, 40}, {10, 40}},
                       SelectionAction::Replace, false);
        INK_CHECK(!s.isEmpty());
        INK_CHECK(s.contains(30, 25));
        printf("select_winding OK\n");
    }
    // Outline segments are axis-aligned boundary runs: their total length
    // stays close to the perimeter (a sloped edge must not emit whole row
    // spans, which is what cross-hatched the marching ants).
    {
        Selection s;
        s.reset(200, 200);
        s.applyPolygon({{20, 20}, {180, 20}, {20, 180}}, SelectionAction::Replace, false);
        float total = 0.0f;
        for (const auto &seg : s.outline()) {
            const float dx = seg[2] - seg[0], dy = seg[3] - seg[1];
            INK_CHECK(dx == 0.0f || dy == 0.0f); // axis-aligned only
            total += std::abs(dx) + std::abs(dy);
        }
        // True perimeter is 160 + 160 + 226 = 546; the staircase walk over the
        // hypotenuse pushes it to ~640. The old bug produced many thousands.
        INK_CHECK(total > 500.0f && total < 900.0f);
        printf("select_outline_tight OK total=%.0f\n", total);
    }
    // Device writes honour the selection clip.
    {
        PaintDevice dev;
        Selection s;
        s.reset(64, 64);
        s.applyPolygon({{16, 16}, {32, 16}, {32, 32}, {16, 32}}, SelectionAction::Replace, false);
        dev.fillRect({0, 0, 64, 64}, {255, 0, 0, 255}, CompositeOp::Normal, nullptr, &s);
        INK_CHECK(dev.pixel(20, 20).a == 255);
        INK_CHECK(dev.pixel(5, 5).a == 0);
        printf("select_clip OK\n");
    }
    printf("selection OK\n");
    return 0;
}
