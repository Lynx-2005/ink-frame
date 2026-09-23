// Incremental projection: projectRect must be pixel-identical to full
// project, and stay cheap with hundreds of layers (tile sparsity).
#include "core/InkImage.h"
#include "test_check.h"
#include <chrono>
#include <cstdio>
#include <vector>

using namespace ink;
using Clock = std::chrono::steady_clock;

static std::vector<uint8_t> readAll(PaintDevice &d, int w, int h) {
    std::vector<uint8_t> buf(size_t(w) * h * 4);
    d.readRect({0, 0, w, h}, buf.data());
    return buf;
}

int main() {
    const int W = 640, H = 480;
    Image img(W, H, 24);
    // Bottom: solid red full-canvas key at 0.
    auto bg = std::make_shared<PaintLayer>("bg");
    bg->setKeyAt(0, true);
    {
        TileMemento m;
        bg->deviceAt(0, true)->fillRect({0, 0, W, H}, {255, 0, 0, 255});
    }
    img.root().addChild(bg);
    // Top: small blue square at frame 0.
    auto fg = std::make_shared<PaintLayer>("fg");
    fg->setKeyAt(0, true);
    {
        TileMemento m;
        fg->deviceAt(0, true)->fillRect({100, 100, 64, 64}, {0, 0, 255, 255});
    }
    img.root().addChild(fg);

    // 1. projectRect(full canvas) == project.
    PaintDevice a, b;
    img.project(0, a);
    img.projectRect(0, {0, 0, W, H}, b);
    INK_CHECK(readAll(a, W, H) == readAll(b, W, H));
    std::printf("rect_eq_full OK\n");

    // 2. Incremental stroke simulation: dab on fg, re-project only the dirty
    // rect into the old projection; must match a from-scratch projection
    // inside AND outside the rect.
    PaintDevice before;
    img.project(0, before);
    Rect dirty;
    {
        TileMemento m;
        float mask[25];
        for (int i = 0; i < 25; ++i) mask[i] = 1.0f;
        dirty = fg->deviceAt(0, true)->applyDab(300, 300, mask, 5, {0, 255, 0, 255}, 1.0f,
                                                CompositeOp::Normal, &m);
    }
    PaintDevice incr;
    incr.blit(before, 0, 0); // seed with the pre-stroke projection
    img.projectRect(0, dirty, incr);
    PaintDevice fresh;
    img.project(0, fresh);
    INK_CHECK(readAll(incr, W, H) == readAll(fresh, W, H));
    std::printf("incremental_eq OK dirty=(%d,%d %dx%d)\n", dirty.x, dirty.y, dirty.w, dirty.h);

    // 3. Scale: 200 sparse layers; incremental dab-rect update vs full project.
    Image big(1920, 1080, 24);
    for (int i = 0; i < 200; ++i) {
        auto l = std::make_shared<PaintLayer>("L" + std::to_string(i));
        l->setKeyAt(0, true);
        TileMemento m;
        int x = (i * 173) % 1800, y = (i * 311) % 1000;
        l->deviceAt(0, true)->fillRect({x, y, 40, 40}, {(uint8_t)(i & 255), 100, 50, 255});
        big.root().addChild(l);
    }
    std::vector<PaintLayer *> layers;
    big.collectPaintLayers(layers);
    PaintDevice full0;
    big.project(0, full0);
    // Simulate a dab on the top layer, then time both refresh strategies.
    Rect dabRect;
    {
        TileMemento m;
        float mask[33 * 33];
        for (int i = 0; i < 33 * 33; ++i) mask[i] = 0.8f;
        dabRect = layers.back()->deviceAt(0, true)->applyDab(960, 540, mask, 33,
                                                             {255, 255, 255, 255}, 1.0f,
                                                             CompositeOp::Normal, &m);
    }
    auto t0 = Clock::now();
    PaintDevice f2;
    big.project(0, f2);
    double fullMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    PaintDevice inc2;
    inc2.blit(full0, 0, 0);
    t0 = Clock::now();
    big.projectRect(0, dabRect, inc2);
    double incMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    INK_CHECK(readAll(inc2, 1920, 1080) == readAll(f2, 1920, 1080));
    std::printf("scale200 full=%.2fms incremental=%.2fms (%.0fx)\n", fullMs, incMs,
                fullMs / std::max(incMs, 1e-6));
    INK_CHECK(incMs < 50.0); // sub-frame budget for the stroke loop
    std::printf("test_scale PASS\n");
    return 0;
}
