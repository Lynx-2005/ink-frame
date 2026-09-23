#include "core/InkRegion.h"
#include "core/InkSelection.h"

namespace ink {

FloatRegion liftRegion(const PaintDevice &src, const Rect &area, const Selection *mask) {
    FloatRegion out;
    if (area.isEmpty())
        return out;
    out.bounds = area;
    out.px.assign(size_t(area.w) * size_t(area.h), PixelRGBA8{0, 0, 0, 0});
    std::vector<uint8_t> buf(size_t(area.w) * size_t(area.h) * 4, 0);
    src.readRect(area, buf.data());
    for (int y = 0; y < area.h; ++y) {
        for (int x = 0; x < area.w; ++x) {
            const size_t i = size_t(y) * size_t(area.w) + size_t(x);
            PixelRGBA8 p{buf[i * 4 + 0], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]};
            if (p.a == 0)
                continue;
            if (mask) {
                const uint8_t cov = mask->coverage(area.x + x, area.y + y);
                if (cov == 0)
                    continue;
                p.a = uint8_t((int(p.a) * int(cov) + 127) / 255);
            }
            out.px[i] = p;
        }
    }
    return out;
}

Rect eraseRegion(PaintDevice &dev, const Rect &area, const Selection *mask, TileMemento *m) {
    if (area.isEmpty())
        return {};
    return dev.fillRect(area, PixelRGBA8{0, 0, 0, 255}, CompositeOp::Erase, m, mask);
}

Rect stampRegion(PaintDevice &dev, const FloatRegion &r, int dx, int dy, const Rect &clipTo,
                 TileMemento *m) {
    if (r.empty())
        return {};
    const Rect target =
        intersectRect({r.bounds.x + dx, r.bounds.y + dy, r.bounds.w, r.bounds.h}, clipTo);
    if (target.isEmpty())
        return {};
    return dev.store().updateTilesInRect(target, m, [&](TileData &t, int ox, int oy, const Rect &c) {
        for (int y = c.y; y < c.y + c.h; ++y) {
            const int sy = y - dy - r.bounds.y;
            if (sy < 0 || sy >= r.bounds.h)
                continue;
            const PixelRGBA8 *srcRow = &r.px[size_t(sy) * size_t(r.bounds.w)];
            PixelRGBA8 *dst = &t.px[(y - oy) * kTileSize + (c.x - ox)];
            for (int x = c.x; x < c.x + c.w; ++x, ++dst) {
                const int sx = x - dx - r.bounds.x;
                if (sx < 0 || sx >= r.bounds.w)
                    continue;
                const PixelRGBA8 s = srcRow[sx];
                if (s.a == 0)
                    continue;
                *dst = PaintDevice::compositeOver(*dst, s);
            }
        }
    });
}

FloatRegion flipRegion(const FloatRegion &r, bool horizontal) {
    FloatRegion out = r;
    if (r.empty())
        return out;
    for (int y = 0; y < r.bounds.h; ++y) {
        for (int x = 0; x < r.bounds.w; ++x) {
            const int sx = horizontal ? r.bounds.w - 1 - x : x;
            const int sy = horizontal ? y : r.bounds.h - 1 - y;
            out.px[size_t(y) * size_t(r.bounds.w) + size_t(x)] =
                r.px[size_t(sy) * size_t(r.bounds.w) + size_t(sx)];
        }
    }
    return out;
}

} // namespace ink
