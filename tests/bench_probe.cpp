// Perf probe: dab throughput + projection + conversion cost (headless).
#include "brush/InkPaintOp.h"
#include "core/InkImage.h"
#include <chrono>
#include <cstdio>

using namespace ink;
using Clock = std::chrono::steady_clock;

int main() {
    AutoBrushTip tip(BrushTipShape{32.0f, 0.75f, 1.0f, 0.08f});
    const auto &mask = tip.mask();
    const int ms = tip.maskSize();
    {
        PaintDevice dev;
        TileMemento m;
        auto t0 = Clock::now();
        const int N = 200;
        for (int i = 0; i < N; ++i)
            dev.applyDab(500 + i, 500, mask.data(), ms, {255, 0, 0, 255}, 1.0f,
                         CompositeOp::Normal, &m);
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("dabs: %d in %.3fs = %.1f dabs/s (tiles=%zu)\n", N, s, N / s,
                    dev.tileCount());
    }
    {
        Image img(1920, 1080, 24);
        auto l = std::make_shared<PaintLayer>("A");
        l->setKeyAt(0, true);
        PaintDevice *d = l->deviceAt(0, true);
        TileMemento m;
        d->fillRect({0, 0, 1920, 1080}, {200, 100, 50, 255}, CompositeOp::Normal, &m);
        img.root().addChild(l);
        PaintDevice out;
        auto t0 = Clock::now();
        const int N = 10;
        for (int i = 0; i < N; ++i) {
            out.clear();
            img.project(0, out);
        }
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("project 1080p: %.3fs each\n", s / N);
    }
    return 0;
}
