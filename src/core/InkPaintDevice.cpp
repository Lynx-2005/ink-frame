#include "core/InkPaintDevice.h"
#include "core/InkSelection.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ink {

static_assert(sizeof(PixelRGBA8) == 4, "PixelRGBA8 must be tightly packed RGBA");

PixelRGBA8 PaintDevice::compositeOver(PixelRGBA8 dst, PixelRGBA8 src, float extraOpacity) {
    if (src.a == 255 && extraOpacity >= 1.0f) return src; // opaque fast path
    float sa = (src.a / 255.0f) * extraOpacity;
    float da = dst.a / 255.0f;
    float outA = sa + da * (1 - sa);
    if (outA <= 1e-6f) return {0,0,0,0};
    auto ch = [&](uint8_t s, uint8_t d) -> uint8_t {
        float v = (s * sa + d * da * (1 - sa)) / outA;
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
    };
    return {ch(src.r,dst.r), ch(src.g,dst.g), ch(src.b,dst.b),
            static_cast<uint8_t>(std::clamp(outA*255.0f, 0.0f, 255.0f))};
}

void PaintDevice::setPixel(int x, int y, PixelRGBA8 p, TileMemento *m) {
    TileData &t = store_.writableTile(floorDiv(x,kTileSize), floorDiv(y,kTileSize), m);
    t.px[modPos(y,kTileSize)*kTileSize + modPos(x,kTileSize)] = p;
    t.markDirty();
}

// Read helper over a tile snapshot list (no locks; tiles are COW-stable).
static PixelRGBA8 snapPixel(
    const std::unordered_map<TilePos, TileShared, TilePosHash> &snap, int x, int y) {
    TilePos p{floorDiv(x, kTileSize), floorDiv(y, kTileSize)};
    auto it = snap.find(p);
    if (it == snap.end()) return {};
    const TileData &t = *it->second;
    return t.px[modPos(y, kTileSize) * kTileSize + modPos(x, kTileSize)];
}

static std::unordered_map<TilePos, TileShared, TilePosHash>
snapshotRect(const TileStore &store, const Rect &r) {
    std::vector<std::pair<TilePos, TileShared>> tiles;
    store.tilesInRect(r, tiles); // single lock
    std::unordered_map<TilePos, TileShared, TilePosHash> snap;
    snap.reserve(tiles.size() * 2 + 1);
    for (auto &e : tiles) snap.emplace(e.first, e.second);
    return snap;
}

Rect PaintDevice::fillRect(const Rect &r, PixelRGBA8 color, CompositeOp op, TileMemento *m,
                           const Selection *clip) {
    if (r.isEmpty()) return {};
    const bool erase = (op == CompositeOp::Erase);
    const bool solid = !clip && (op == CompositeOp::Normal || erase) && color.a == 255;
    const PixelRGBA8 solidPx = erase ? PixelRGBA8{0,0,0,0} : color;
    return store_.updateTilesInRect(r, m, [&](TileData &t, int ox, int oy, const Rect &c) {
        for (int y = c.y; y < c.y + c.h; ++y) {
            PixelRGBA8 *row = &t.px[(y - oy) * kTileSize + (c.x - ox)];
            if (solid) {
                for (int x = c.x; x < c.x + c.w; ++x) *row++ = solidPx;
                continue;
            }
            for (int x = c.x; x < c.x + c.w; ++x, ++row) {
                float cov = clip ? float(clip->coverage(x, y)) / 255.0f : 1.0f;
                if (cov <= 0.002f) continue;
                if (erase) {
                    row->a = static_cast<uint8_t>(float(row->a) * (1.0f - cov));
                } else {
                    PixelRGBA8 s = color;
                    s.a = static_cast<uint8_t>(std::clamp(float(s.a) * cov, 0.0f, 255.0f));
                    if (s.a == 0) continue;
                    *row = compositeOver(*row, s);
                }
            }
        }
    });
}

Rect PaintDevice::blit(const PaintDevice &src, int dx, int dy, float opacity,
                       CompositeOp op, TileMemento *m) {
    Rect se = src.extent();
    if (se.isEmpty()) return {};
    return blitRect(src, se, dx + se.x, dy + se.y, opacity, op, m);
}

Rect PaintDevice::blitRect(const PaintDevice &src, const Rect &srcRect, int dx, int dy,
                           float opacity, CompositeOp op, TileMemento *m) {
    if (srcRect.isEmpty()) return {};
    Rect dirty{dx, dy, srcRect.w, srcRect.h};
    if (dirty.isEmpty()) return {};
    // Snapshot source tiles first (single lock, no lock-ordering hazard with
    // the destination update below).
    auto snap = snapshotRect(src.store_, srcRect);
    return store_.updateTilesInRect(dirty, m, [&](TileData &t, int ox, int oy, const Rect &c) {
        for (int y = c.y; y < c.y + c.h; ++y) {
            PixelRGBA8 *row = &t.px[(y - oy) * kTileSize + (c.x - ox)];
            for (int x = c.x; x < c.x + c.w; ++x, ++row) {
                // dst(x,y) <- src(srcRect.x + x - dx, srcRect.y + y - dy)
                PixelRGBA8 s = snapPixel(snap, srcRect.x + x - dx, srcRect.y + y - dy);
                if (s.a == 0) continue;
                if (op == CompositeOp::Erase) {
                    float cut = (s.a / 255.0f) * opacity;
                    row->a = static_cast<uint8_t>(row->a * (1 - cut));
                } else if (s.a == 255 && opacity >= 1.0f) {
                    *row = s; // opaque copy, no float math
                } else {
                    *row = compositeOver(*row, s, opacity);
                }
            }
        }
    });
}

Rect PaintDevice::applyDab(float cx, float cy, const float *mask, int maskSize,
                           PixelRGBA8 color, float opacity, CompositeOp op,
                           TileMemento *m, const Selection *clip) {
    int r = maskSize / 2;
    int x0 = static_cast<int>(std::floor(cx)) - r;
    int y0 = static_cast<int>(std::floor(cy)) - r;
    Rect dirty{x0, y0, maskSize, maskSize};
    const bool erase = (op == CompositeOp::Erase);
    return store_.updateTilesInRect(dirty, m, [&](TileData &t, int ox, int oy, const Rect &c) {
        for (int y = c.y; y < c.y + c.h; ++y) {
            const float *mrow = &mask[(y - y0) * maskSize + (c.x - x0)];
            PixelRGBA8 *row = &t.px[(y - oy) * kTileSize + (c.x - ox)];
            for (int x = c.x; x < c.x + c.w; ++x, ++mrow, ++row) {
                float a = *mrow * opacity;
                if (clip)
                    a *= float(clip->coverage(x, y)) / 255.0f;
                if (a <= 0.003f) continue;
                if (erase) {
                    row->a = static_cast<uint8_t>(row->a * (1 - a));
                } else {
                    PixelRGBA8 s = color;
                    s.a = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                    *row = compositeOver(*row, s);
                }
            }
        }
    });
}

void PaintDevice::readRect(const Rect &r, uint8_t *rgbaOut) const {
    if (r.isEmpty() || !rgbaOut) return;
    std::memset(rgbaOut, 0, size_t(r.w) * size_t(r.h) * 4);
    std::vector<std::pair<TilePos, TileShared>> tiles;
    store_.tilesInRect(r, tiles); // single lock
    for (auto &[pos, t] : tiles) {
        const int ox = pos.tx * kTileSize, oy = pos.ty * kTileSize;
        const int x0 = std::max(r.x, ox), y0 = std::max(r.y, oy);
        const int x1 = std::min(r.x + r.w, ox + kTileSize), y1 = std::min(r.y + r.h, oy + kTileSize);
        for (int y = y0; y < y1; ++y) {
            uint8_t *dst = rgbaOut + (size_t(y - r.y) * r.w + (x0 - r.x)) * 4;
            const PixelRGBA8 *src = &t->px[(y - oy) * kTileSize + (x0 - ox)];
            std::memcpy(dst, src, size_t(x1 - x0) * 4);
        }
    }
}

} // namespace ink
