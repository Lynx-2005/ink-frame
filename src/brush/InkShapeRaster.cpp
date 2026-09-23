#include "brush/InkShapeRaster.h"
#include "brush/InkPaintOp.h" // StrokeInterpolator
#include "core/InkSelection.h"
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

Rect stampPoints(PaintDevice &dev, const AutoBrushTip &tip, PixelRGBA8 color, float opacity,
                 CompositeOp op, TileMemento *memento, const Selection *clip,
                 const std::vector<RasterPoint> &pts) {
    StrokeInterpolator interp(tip.spacingPx());
    Rect acc;
    for (const RasterPoint &pt : pts) {
        TabletState s;
        s.x = float(pt.x);
        s.y = float(pt.y);
        s.pressure = 1.0f; // shapes are not pressure-modulated
        for (const Dab &d : interp.push(s, 1.0f, 1.0f))
            acc = uniteRect(acc, dev.applyDab(d.x, d.y, tip.mask().data(), tip.maskSize(),
                                              color, opacity, op, memento, clip));
    }
    return acc;
}

} // namespace

Rect strokePolyline(PaintDevice &dev, const std::vector<RasterPoint> &pts, const AutoBrushTip &tip,
                    PixelRGBA8 color, float opacity, CompositeOp op, TileMemento *memento,
                    const Selection *clip) {
    if (pts.empty())
        return {};
    return stampPoints(dev, tip, color, opacity, op, memento, clip, pts);
}

Rect strokeRect(PaintDevice &dev, double x0, double y0, double x1, double y1, const AutoBrushTip &tip,
                PixelRGBA8 color, float opacity, CompositeOp op, TileMemento *memento,
                const Selection *clip) {
    const std::vector<RasterPoint> pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}};
    return stampPoints(dev, tip, color, opacity, op, memento, clip, pts);
}

Rect strokeEllipse(PaintDevice &dev, double cx, double cy, double rx, double ry, const AutoBrushTip &tip,
                   PixelRGBA8 color, float opacity, CompositeOp op, TileMemento *memento,
                   const Selection *clip) {
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-3 || ry < 1e-3)
        return {};
    // Sample the perimeter at ~half the dab spacing so the spacing walker
    // never has to bridge a corner.
    const double spacing = std::max(1.0, double(tip.spacingPx()));
    const double h = std::pow(rx - ry, 2) / std::pow(rx + ry, 2);
    double perimeter = M_PI * (rx + ry) * (1.0 + 3.0 * h / (10.0 + std::sqrt(4.0 - 3.0 * h)));
    if (!(perimeter > 0))
        perimeter = 1.0;
    int steps = std::clamp(int(std::ceil(perimeter / std::max(1.0, spacing * 0.5))), 16, 4096);
    std::vector<RasterPoint> pts;
    pts.reserve(size_t(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double a = 2.0 * M_PI * double(i) / double(steps);
        pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    return stampPoints(dev, tip, color, opacity, op, memento, clip, pts);
}

Rect fillEllipse(PaintDevice &dev, double cx, double cy, double rx, double ry, PixelRGBA8 color,
                 CompositeOp op, TileMemento *memento, const Selection *clip) {
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1e-3 || ry < 1e-3)
        return {};
    const int y0 = int(std::floor(cy - ry));
    const int y1 = int(std::ceil(cy + ry));
    Rect acc;
    for (int y = y0; y < y1; ++y) {
        const double dy = (y + 0.5 - cy) / ry;
        if (dy <= -1.0 || dy >= 1.0)
            continue;
        const double half = rx * std::sqrt(1.0 - dy * dy);
        const int xa = int(std::ceil(cx - half - 0.5));
        const int xb = int(std::floor(cx + half - 0.5));
        if (xb < xa)
            continue;
        acc = uniteRect(acc, dev.fillRect({xa, y, xb - xa + 1, 1}, color, op, memento, clip));
    }
    return acc;
}

} // namespace ink
