#pragma once
// Flood fill for the fill tool: contiguous span fill or global "fill similar
// pixels", sampled from any paint device (the merged projection or a layer),
// written into the target device and clipped by a selection when one exists.
#include "core/InkPaintDevice.h"
#include "core/InkSelection.h"
#include "core/InkTileStore.h"

namespace ink {

struct FloodFillParams {
    int tolerance{0};                    // 0..255 per-channel distance
    bool contiguous{true};
    const Selection *clip{nullptr};
};

// Fills the region of `source` connected to (sx, sy) (or every similar pixel
// when !contiguous) with `color` in `target`. Returns the dirty rect.
Rect floodFill(PaintDevice &target, const PaintDevice &source, const Rect &region,
               int sx, int sy, PixelRGBA8 color, const FloodFillParams &params,
               TileMemento *memento);

} // namespace ink
