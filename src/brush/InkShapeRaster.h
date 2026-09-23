#pragma once
// Shape rasterization for the shape tools: stroke a polyline / rectangle /
// ellipse with the same tip and spacing walk the freehand engine uses
// (StrokeInterpolator), plus a scanline ellipse fill. Clean-room, no Qt.
#include "brush/InkBrushTip.h"
#include "core/InkPaintDevice.h"
#include "core/InkTileStore.h"
#include <vector>

namespace ink {

struct RasterPoint {
    double x{0}, y{0};
};

// Stamp the tip along the polyline (spacing taken from the tip). Returns the
// dirty rect; writes capture into `memento` when one is given.
Rect strokePolyline(PaintDevice &dev, const std::vector<RasterPoint> &pts, const AutoBrushTip &tip,
                    PixelRGBA8 color, float opacity = 1.0f, CompositeOp op = CompositeOp::Normal,
                    TileMemento *memento = nullptr);

Rect strokeRect(PaintDevice &dev, double x0, double y0, double x1, double y1, const AutoBrushTip &tip,
                PixelRGBA8 color, float opacity = 1.0f, CompositeOp op = CompositeOp::Normal,
                TileMemento *memento = nullptr);

Rect strokeEllipse(PaintDevice &dev, double cx, double cy, double rx, double ry, const AutoBrushTip &tip,
                   PixelRGBA8 color, float opacity = 1.0f, CompositeOp op = CompositeOp::Normal,
                   TileMemento *memento = nullptr);

// Scanline fill of an axis-aligned ellipse (rectangles use PaintDevice::fillRect).
Rect fillEllipse(PaintDevice &dev, double cx, double cy, double rx, double ry, PixelRGBA8 color,
                 CompositeOp op = CompositeOp::Normal, TileMemento *memento = nullptr);

} // namespace ink
