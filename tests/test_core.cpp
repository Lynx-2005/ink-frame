// Headless engine tests: tile store COW + undo, paint device, image projection.
#include "brush/InkBrushTip.h"
#include "core/InkImage.h"
#include "core/InkStroke.h"
#include "test_check.h"
#include <cstdio>

using namespace ink;

int main() {
    // Tile COW + memento
    {
        PaintDevice d;
        TileMemento m;
        d.fillRect({0,0,100,100}, {255,0,0,255}, CompositeOp::Normal, &m);
        INK_CHECK(d.pixel(10,10).r == 255);
        INK_CHECK(!m.empty());
        d.store().restore(m);
        INK_CHECK(d.pixel(10,10).a == 0);
        printf("tile_memento OK (tiles=%zu)\n", d.tileCount());
    }
    // Dab + composite
    {
        PaintDevice d;
        AutoBrushTip tip; // fwd-declared via InkPaintDevice? use raw mask
        (void)tip;
        float mask[9] = {0,0.5,0, 0.5,1,0.5, 0,0.5,0};
        TileMemento m;
        d.applyDab(32, 32, mask, 3, {0,0,255,255}, 1.0f, CompositeOp::Normal, &m);
        INK_CHECK(d.pixel(32,32).b > 200);
        printf("dab OK\n");
    }
    // Image projection with holds
    {
        Image img(64, 64, 24);
        auto l = std::make_shared<PaintLayer>("A");
        l->setKeyAt(0, true);
        l->deviceAt(0, true)->fillRect({0,0,64,64}, {255,0,0,255});
        l->setKeyAt(10, true);
        l->deviceAt(10, true)->fillRect({0,0,64,64}, {0,0,255,255});
        img.root().addChild(l);
        PaintDevice out;
        img.project(5, out);
        INK_CHECK(out.pixel(4,4).r == 255); // hold of key 0
        img.project(10, out);
        INK_CHECK(out.pixel(4,4).b == 255);
        printf("projection_hold OK\n");
    }
    // Undo stack
    {
        UndoStack u;
        PaintDevice d;
        TileMemento m;
        d.fillRect({0,0,8,8}, {1,2,3,255}, CompositeOp::Normal, &m);
        StrokeCommand c; c.name = "t"; c.targets.push_back({&d, std::move(m)});
        u.push(std::move(c));
        u.undo();
        INK_CHECK(d.pixel(0,0).a == 0);
        printf("undo OK\n");
    }
    printf("test_core PASS\n");
    return 0;
}
