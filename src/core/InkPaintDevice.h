#pragma once
// Raster paint device: owns a TileStore, exposes pixel ops + dab compositing.
// Production: dirty-rect tracking for canvas invalidation + projection,
// per-stroke mementos, fast paths for solid tiles.
#include "core/InkTileStore.h"
#include <functional>
#include <memory>

namespace ink {

enum class CompositeOp { Normal, Erase, Multiply, Screen, Behind };

class PaintDevice {
public:
    PaintDevice() = default;
    PaintDevice(const PaintDevice &) = delete;
    PaintDevice &operator=(const PaintDevice &) = delete;

    PixelRGBA8 pixel(int x, int y) const { return store_.pixel(x, y); }
    void setPixel(int x, int y, PixelRGBA8 p, TileMemento *m = nullptr);

    // Fill rect with color (honours erase->clear). Returns dirty rect.
    Rect fillRect(const Rect &r, PixelRGBA8 color, CompositeOp op = CompositeOp::Normal,
                  TileMemento *m = nullptr);
    // Composite src-over tile region from another device.
    Rect blit(const PaintDevice &src, int dx, int dy, float opacity = 1.0f,
              CompositeOp op = CompositeOp::Normal, TileMemento *m = nullptr);
    // Same, but only the srcRect sub-rectangle of src (for clipped
    // incremental projection). dx/dy offset srcRect's top-left to dst.
    Rect blitRect(const PaintDevice &src, const Rect &srcRect, int dx, int dy,
                  float opacity = 1.0f, CompositeOp op = CompositeOp::Normal,
                  TileMemento *m = nullptr);

    // Dab stamping used by brush engine: circular soft mask * color * opacity.
    // maskSize x maskSize float mask in [0,1], centered at (cx,cy).
    Rect applyDab(float cx, float cy, const float *mask, int maskSize,
                  PixelRGBA8 color, float opacity, CompositeOp op,
                  TileMemento *m = nullptr);

    // Bulk RGBA read of rect into a w*h*4 buffer (for canvas upload).
    // Single lock + memcpy per tile — use instead of per-pixel pixel().
    void readRect(const Rect &r, uint8_t *rgbaOut) const;

    void clear() { store_.clear(); }
    Rect extent() const { return store_.extent(); }
    size_t tileCount() const { return store_.tileCount(); }
    const TileStore &store() const { return store_; }
    TileStore &store() { return store_; }

    // Snapshot share (COW): projection cache / onion skin read without copy.
    // Writes after sharing trigger per-tile detach automatically.
    // (tiles are shared_ptr; no explicit call needed — documented contract.)

    static PixelRGBA8 compositeOver(PixelRGBA8 dst, PixelRGBA8 src, float extraOpacity = 1.0f);

private:
    TileStore store_;
};

} // namespace ink
