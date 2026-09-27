#include "core/InkPaintDevice.h"
#include "core/InkSelection.h"
#include "pigment/InkColor.h"
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

// Tile snapshot helper below: blitRect/onion resolve source tiles once per
// destination tile (no per-pixel hash lookups).

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
    // One destination clip's worth of pixels from the resolved 2x2 source
    // tiles. The mapping is translation-only, so each row crosses at most one
    // source tile boundary (`bx`); local-x runs linearly on each side of the
    // split instead of paying an integer modulo per pixel (the old loop spent
    // ~half its time in `%`). `by` picks the tile row once per pixel row.
    auto put = [&](PixelRGBA8 &d, PixelRGBA8 s) {
        if (s.a == 0) return;
        if (op == CompositeOp::Erase) {
            float cut = (s.a / 255.0f) * opacity;
            d.a = static_cast<uint8_t>(d.a * (1 - cut));
        } else if (op == CompositeOp::Multiply) {
            d = composite::multiply(d, s, opacity);
        } else if (op == CompositeOp::Screen) {
            d = composite::screen(d, s, opacity);
        } else if (s.a == 255 && opacity >= 1.0f) {
            d = s; // opaque copy, no float math
        } else {
            d = compositeOver(d, s, opacity);
        }
    };
    auto stampClip = [&](TileData &t, int ox, int oy, const Rect &c,
                         const TileData *sT[2][2], int stx0, int sty0) {
        const int bx = (stx0 + 1) * kTileSize; // first src tile boundary in x
        const int by = (sty0 + 1) * kTileSize; // ... and y (clip is at most
        // one tile tall, so every row falls on exactly one side of `by`)
        for (int y = c.y; y < c.y + c.h; ++y) {
            PixelRGBA8 *prow = &t.px[(y - oy) * kTileSize + (c.x - ox)];
            const int sy = srcRect.y + y - dy;
            const TileData *const *trow = sT[(sy >= by) ? 1 : 0];
            const int ly = modPos(sy, kTileSize);
            const int sxRow = srcRect.x + c.x - dx;
            // Column (row-relative) where the right tile starts; bx is
            // tile-aligned so the right segment's local-x restarts at 0, and
            // the left segment provably stays in-bounds (length <= 64 minus
            // its start offset within the tile).
            int split = bx - sxRow;
            if (split < 0) split = 0;
            if (split > c.w) split = c.w;
            int x = 0;
            PixelRGBA8 *px = prow;
            if (trow[0]) {
                const PixelRGBA8 *base = &trow[0]->px[ly * kTileSize];
                int lx = modPos(sxRow, kTileSize);
                for (; x < split; ++x, ++px, ++lx) put(*px, base[lx]);
            } else {
                x = split;
                px += split;
            }
            if (trow[1]) {
                const PixelRGBA8 *base = &trow[1]->px[ly * kTileSize];
                int lx = 0;
                for (; x < c.w; ++x, ++px, ++lx) put(*px, base[lx]);
            }
        }
    };
    // Fast path: source spans of <= 2x2 tiles (dots, dirty rects, thumbnails)
    // resolve with one lock, a stack buffer and linear probes — no
    // unordered_map or vector allocation per layer per frame.
    std::pair<TilePos, TileShared> small[4];
    const int nSmall = src.store_.snapshotSmall(srcRect, small);
    if (nSmall >= 0) {
        auto findSmall = [&](int tx, int ty) -> const TileData * {
            for (int i = 0; i < nSmall; ++i)
                if (small[i].first.tx == tx && small[i].first.ty == ty)
                    return small[i].second.get();
            return nullptr;
        };
        return store_.updateTilesInRect(dirty, m, [&](TileData &t, int ox, int oy, const Rect &c) {
            const int sx0 = srcRect.x + c.x - dx, sy0 = srcRect.y + c.y - dy;
            const int stx0 = floorDiv(sx0, kTileSize), sty0 = floorDiv(sy0, kTileSize);
            const int stx1 = floorDiv(sx0 + c.w - 1, kTileSize),
                      sty1 = floorDiv(sy0 + c.h - 1, kTileSize);
            const TileData *sT[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
            for (int j = sty0; j <= sty1; ++j)
                for (int i = stx0; i <= stx1; ++i) sT[j - sty0][i - stx0] = findSmall(i, j);
            stampClip(t, ox, oy, c, sT, stx0, sty0);
        });
    }
    // Wide-span fallback: the previous unordered_map snapshot path.
    // Snapshot source tiles first (single lock, no lock-ordering hazard with
    // the destination update below).
    auto snap = snapshotRect(src.store_, srcRect);
    return store_.updateTilesInRect(dirty, m, [&](TileData &t, int ox, int oy, const Rect &c) {
        const int sx0 = srcRect.x + c.x - dx, sy0 = srcRect.y + c.y - dy;
        const int stx0 = floorDiv(sx0, kTileSize), sty0 = floorDiv(sy0, kTileSize);
        const int stx1 = floorDiv(sx0 + c.w - 1, kTileSize),
                  sty1 = floorDiv(sy0 + c.h - 1, kTileSize);
        const TileData *sT[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};
        for (int j = sty0; j <= sty1; ++j)
            for (int i = stx0; i <= stx1; ++i) {
                auto it = snap.find(TilePos{i, j});
                sT[j - sty0][i - stx0] = (it == snap.end()) ? nullptr : it->second.get();
            }
        stampClip(t, ox, oy, c, sT, stx0, sty0);
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

void PaintDevice::readRect(const Rect &r, uint8_t *rgbaOut) const {    if (r.isEmpty() || !rgbaOut) return;
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

Rect PaintDevice::contentBounds() const {
    const Rect ext = store_.extent();
    if (ext.isEmpty()) return {};
    std::vector<std::pair<TilePos, TileShared>> tiles;
    store_.tilesInRect(ext, tiles); // single lock
    int x0 = ext.x + ext.w, y0 = ext.y + ext.h, x1 = ext.x, y1 = ext.y;
    for (auto &[pos, t] : tiles) {
        if (!t) continue;
        const int ox = pos.tx * kTileSize, oy = pos.ty * kTileSize;
        const int xa = std::max(ext.x, ox), ya = std::max(ext.y, oy);
        const int xb = std::min(ext.x + ext.w, ox + kTileSize),
                  yb = std::min(ext.y + ext.h, oy + kTileSize);
        for (int y = ya; y < yb; ++y) {
            const PixelRGBA8 *row = &t->px[(y - oy) * kTileSize + (xa - ox)];
            for (int x = xa; x < xb; ++x, ++row) {
                if (row->a == 0) continue;
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y + 1 > y1) y1 = y + 1;
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace ink
