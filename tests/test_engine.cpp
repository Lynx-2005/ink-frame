// Brush + pigment + animation headless tests.
#include "animation/InkAnimation.h"
#include "animation/InkKeyframe.h"
#include "animation/InkTimelineModel.h"
#include "brush/InkBrushTip.h"
#include "brush/InkPaintOp.h"
#include "brush/InkSensors.h"
#include "core/InkPaintDevice.h"
#include "pigment/InkColor.h"
#include "test_check.h"
#include <cstdio>

using namespace ink;

int main() {
    // Brush tip mask
    {
        AutoBrushTip tip(BrushTipShape{32.0f, 0.8f, 1.0f, 0.1f});
        INK_CHECK(tip.maskSize() % 2 == 1);
        float center = tip.mask()[(tip.maskSize()/2)*tip.maskSize() + tip.maskSize()/2];
        INK_CHECK(center > 0.99f);
        INK_CHECK(tip.mask()[0] == 0.0f);
        printf("tip OK size=%d\n", tip.maskSize());
    }
    // Interpolator spacing
    {
        StrokeInterpolator it(5.0f);
        TabletState s; s.x = 0; s.y = 0;
        auto d0 = it.push(s, 1, 1);
        s.x = 12; auto d1 = it.push(s, 1, 1);
        INK_CHECK(d1.size() >= 2); // 12px / 5px spacing
        printf("interp OK dabs=%zu\n", d1.size());
    }
    // Pixel brush stroke paints
    {
        PaintDevice dev;
        PixelBrushOp op(AutoBrushTip(BrushTipShape{16.0f,0.7f,1.0f,0.05f}), BrushDynamics{});
        TileMemento m;
        op.beginStroke({&dev, {255,0,0,255}, CompositeOp::Normal, &m});
        for (int i = 0; i < 10; ++i) {
            StrokeEvent e; e.state.x = float(i*2); e.state.y = 50; e.state.pressure = 0.8f;
            op.addEvent(e);
        }
        op.endStroke();
        INK_CHECK(dev.extent().w > 0);
        INK_CHECK(!m.empty());
        printf("brush_stroke OK tiles=%zu\n", dev.tileCount());
    }
    // Pigment composite
    {
        PixelRGBA8 d{255,0,0,255}, s{0,0,255,128};
        PixelRGBA8 o = composite::normal(d, s, 1.0f);
        INK_CHECK(o.a == 255 && o.b > o.r); // blue over red dominates
        PixelRGBA8 m = composite::multiply({255,255,255,255}, {128,128,128,255});
        INK_CHECK(m.r < 200 && m.r > 100);
        // sRGB roundtrip
        PixelRGBA8 p{200,100,50,255};
        PixelRGBA8 q = toSrgb(toLinear(p));
        INK_CHECK(abs((int)q.r - (int)p.r) <= 2);
        printf("pigment OK\n");
    }
    // Keyframes
    {
        RasterChannel rc; rc.setKey(0); rc.setKey(10);
        INK_CHECK(rc.activeAt(5) == 0 && rc.activeAt(10) == 10 && rc.activeAt(11) == 10);
        INK_CHECK(rc.prevKey(10) == 0 && rc.nextKey(0) == 10);
        ScalarChannel sc; sc.setKey(0, 0.0f, Interp::Linear); sc.setKey(10, 1.0f);
        INK_CHECK(sc.valueAt(0) == 0.0f && abs(sc.valueAt(5) - 0.5f) < 1e-5);
        sc.setKey(0, 0.25f, Interp::Hold);
        INK_CHECK(sc.valueAt(5) == 0.25f);
        printf("keyframes OK\n");
    }
    // Timeline model
    {
        TimelineModel tm; tm.addLayer("a", "A"); tm.setKey("a", 3);
        INK_CHECK(tm.activeKey("a", 5) == 3);
        tm.setCurrentFrame(5); INK_CHECK(tm.currentFrame() == 5);
        printf("timeline OK\n");
    }
    // FrameCache + playback
    {
        FrameCache fc(8, 1);
        fc.setRenderFn([](int f){ auto d = std::make_shared<PaintDevice>();
            d->fillRect({0,0,4,4}, {(uint8_t)(f&255),0,0,255}); return d; });
        fc.requestRange(0, 4);
        fc.waitFor(2);
        INK_CHECK(fc.get(2) != nullptr);
        INK_CHECK(fc.get(2)->pixel(0,0).r == 2);
        PlaybackEngine pb(24); pb.setRange(0, 10); pb.play();
        auto [f, ch] = pb.advance(1.0/24.0);
        INK_CHECK(f == 1);
        printf("cache_playback OK\n");
    }
    // Onion tint
    {
        PaintDevice canvas, frame;
        frame.fillRect({0,0,4,4}, {255,255,255,255});
        compositeOnionSkin(canvas, frame, {255,0,0,255}, 0.5f);
        PixelRGBA8 p = canvas.pixel(0,0);
        INK_CHECK(p.r > p.b && p.a > 0);
        printf("onion OK\n");
    }
    printf("test_engine PASS\n");
    return 0;
}
