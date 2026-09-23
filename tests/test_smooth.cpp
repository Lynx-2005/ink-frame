// Stroke smoothness, honestly measured (all checks are NDEBUG-proof):
//  - flattener alone turns a sparse 16-gon into a circle-true dense path
//  - speed-adaptive stabilizer kills slow-speed jitter without shrinking
//    fast circles (the failure mode of naive window averaging)
//  - multipliers lerp along the spacing walk; full op paints a closed ring.
#include "brush/InkPaintOp.h"
#include "brush/InkSmoother.h"
#include "test_check.h"
#include <cmath>
#include <vector>

using namespace ink;
static const double kPi = 3.14159265358979323846;

int main() {
    // 1. Dejitter: +/-2px noise on a slow line must flatten (120 Hz samples).
    {
        Stabilizer st(SmoothingConfig{SmoothingMode::Weighted, 0.5f, true});
        st.reset({0, 0, 0.8f, 0.0});
        double maxDev = 0;
        for (int i = 1; i <= 60; ++i) {
            RawSample o = st.push({double(i), (i % 2 == 0) ? 2.0 : -2.0, 0.8f, i / 120.0});
            if (i >= 30) maxDev = std::max(maxDev, std::abs(o.y));
        }
        std::printf("dejitter maxDev=%f\n", maxDev);
        INK_CHECK(maxDev < 0.8);
    }
    // 2. Sparse polygon -> circle via the curve fitter alone (no stabilizer
    // to hide behind): 16 points, 22.5 deg apart, r=100.
    {
        const double cx = 500, cy = 500, r = 100;
        CurveFlattener fl(0.25);
        std::vector<PathPoint> path;
        for (int k = 0; k <= 16; ++k) {
            double a = k * 2 * kPi / 16.0;
            PathPoint p{cx + r * std::cos(a), cy + r * std::sin(a), 1, 1};
            if (k == 0) {
                fl.reset(p);
                path.push_back(p);
            } else {
                for (auto &pt : fl.push(p)) path.push_back(pt);
            }
        }
        for (auto &pt : fl.finish()) path.push_back(pt);
        INK_CHECK(path.size() > 40);
        double maxDev = 0, length = 0;
        for (size_t i = 0; i < path.size(); ++i) {
            maxDev = std::max(maxDev, std::abs(std::hypot(path[i].x - cx, path[i].y - cy) - r));
            if (i) length += std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
        }
        double err = std::abs(length - 2 * kPi * r) / (2 * kPi * r);
        std::printf("flat16 pts=%zu maxDev=%.2f lenErr=%.2f%%\n", path.size(), maxDev, 100 * err);
        INK_CHECK(maxDev < 3.0);
        INK_CHECK(err < 0.05);
    }
    // 3. Fast dense circle through the FULL path (stabilizer + fitter) at a
    // realistic 120 Hz: 72 samples/rev must not shrink (radius loss < 3px)
    // and must stay round (radial deviation < 3px).
    {
        const double cx = 500, cy = 500, r = 100;
        Stabilizer st(SmoothingConfig{SmoothingMode::Weighted, 0.3f, false});
        CurveFlattener fl(0.25);
        std::vector<PathPoint> path;
        for (int k = 0; k <= 72; ++k) {
            double a = k * 2 * kPi / 72.0;
            RawSample raw{cx + r * std::cos(a), cy + r * std::sin(a), 0.8f, k / 120.0};
            if (k == 0) {
                st.reset(raw);
                RawSample s0 = st.push(raw);
                PathPoint p0{s0.x, s0.y, 1, 1};
                fl.reset(p0);
                path.push_back(p0);
            } else {
                RawSample s = st.push(raw);
                for (auto &pt : fl.push({s.x, s.y, 1, 1})) path.push_back(pt);
            }
        }
        for (auto &pt : fl.finish()) path.push_back(pt);
        double maxDev = 0, meanR = 0;
        for (auto &pt : path) {
            double rr = std::hypot(pt.x - cx, pt.y - cy);
            meanR += rr;
            maxDev = std::max(maxDev, std::abs(rr - r));
        }
        meanR /= path.size();
        std::printf("fast72 pts=%zu maxDev=%.2f shrink=%.2f\n", path.size(), maxDev, r - meanR);
        INK_CHECK(maxDev < 3.0);
        INK_CHECK(r - meanR < 3.0);
    }
    // 4. Multiplier lerp: dabs between low- and high-pressure events ramp.
    {
        StrokeInterpolator it(5.0f);
        it.reset(0, 0, 0.2f, 0.2f);
        TabletState b;
        b.x = 20;
        b.y = 0;
        auto dabs = it.push(b, 1.0f, 1.0f);
        INK_CHECK(dabs.size() >= 3);
        for (size_t i = 1; i < dabs.size(); ++i)
            INK_CHECK(dabs[i].sizeMul >= dabs[i - 1].sizeMul);
        INK_CHECK(dabs.back().sizeMul > 0.9f);
        std::printf("pressure_lerp dabs=%zu\n", dabs.size());
    }
    // 5. End to end: 24-event freehand circle paints a closed ring.
    {
        PaintDevice dev;
        BrushDynamics dyn;
        dyn.smoothing = SmoothingConfig{SmoothingMode::Weighted, 0.2f, true};
        PixelBrushOp op(AutoBrushTip(BrushTipShape{10.0f, 0.9f, 1.0f, 0.05f}), dyn);
        TileMemento m;
        op.beginStroke({&dev, {255, 0, 0, 255}, CompositeOp::Normal, &m});
        const double cx = 200, cy = 200, r = 60;
        for (int k = 0; k <= 24; ++k) {
            double a = k * 2 * kPi / 24.0;
            StrokeEvent e;
            e.state.x = float(cx + r * std::cos(a));
            e.state.y = float(cy + r * std::sin(a));
            e.state.pressure = 0.9f;
            e.timeSec = k / 120.0;
            op.addEvent(e);
        }
        op.endStroke();
        int hit = 0;
        for (int k = 0; k < 16; ++k) {
            double a = (k + 0.5) * 2 * kPi / 16.0;
            PixelRGBA8 px = dev.pixel(int(cx + r * std::cos(a)), int(cy + r * std::sin(a)));
            if (px.r > 150 && px.a > 150) ++hit;
        }
        INK_CHECK(hit >= 14);
        INK_CHECK(dev.pixel(int(cx), int(cy)).a < 30);
        std::printf("ring hit=%d/16\n", hit);
    }
    std::printf("test_smooth PASS\n");
    return 0;
}
