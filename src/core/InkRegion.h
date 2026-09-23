#pragma once
// Floating (lifted) pixel regions: the machinery behind the move tool.
// A region is copied out of an image, erased from the source under a mask and
// composited back at an offset - all through the tile store so a drag stays
// cheap and the undo step is a single memento.
#include "core/InkPaintDevice.h"
#include "core/InkTileStore.h"
#include <vector>

namespace ink {

class Selection;

struct FloatRegion {
    Rect bounds;                // document-space bounds of the lifted pixels
    std::vector<PixelRGBA8> px; // bounds.w * bounds.h, row major
    bool empty() const { return bounds.isEmpty() || px.empty(); }
};

// Copy the pixels under `mask` (nullptr = everything inside `area`).
FloatRegion liftRegion(const PaintDevice &src, const Rect &area, const Selection *mask);

// Clear `area` (masked) to transparent.
Rect eraseRegion(PaintDevice &dev, const Rect &area, const Selection *mask, TileMemento *m);

// Composite the lifted region at (bounds.x + dx, bounds.y + dy), clipped to
// `clipTo` (the document rect). Pixels are kept opaque-neutral on overlap.
Rect stampRegion(PaintDevice &dev, const FloatRegion &r, int dx, int dy, const Rect &clipTo,
                 TileMemento *m);

// Mirror a lifted region in place (used by the flip actions).
FloatRegion flipRegion(const FloatRegion &r, bool horizontal);

} // namespace ink
