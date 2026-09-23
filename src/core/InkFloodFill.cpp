#include "core/InkFloodFill.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ink {

namespace {

struct Run {
    int y{0}, x0{0}, x1{0}; // x1 exclusive
};

inline bool similar(PixelRGBA8 a, PixelRGBA8 b, int tolerance) {
    return std::abs(int(a.r) - int(b.r)) <= tolerance && std::abs(int(a.g) - int(b.g)) <= tolerance &&
           std::abs(int(a.b) - int(b.b)) <= tolerance && std::abs(int(a.a) - int(b.a)) <= tolerance;
}

} // namespace

Rect floodFill(PaintDevice &target, const PaintDevice &source, const Rect &region, int sx, int sy,
               PixelRGBA8 color, const FloodFillParams &params, TileMemento *memento) {
    if (region.isEmpty() || !region.contains(sx, sy) || color.a == 0)
        return {};

    std::vector<uint8_t> src(size_t(region.w) * size_t(region.h) * 4);
    source.readRect(region, src.data());
    auto at = [&](int x, int y) {
        const size_t i = (size_t(y - region.y) * size_t(region.w) + size_t(x - region.x)) * 4;
        return PixelRGBA8{src[i], src[i + 1], src[i + 2], src[i + 3]};
    };

    const PixelRGBA8 seed = at(sx, sy);
    if (similar(seed, color, 0))
        return {}; // already exactly the requested color: nothing to do

    const int tolerance = std::clamp(params.tolerance, 0, 255);
    std::vector<uint8_t> hit(size_t(region.w) * size_t(region.h), 0);
    auto matches = [&](int x, int y) { return similar(at(x, y), seed, tolerance); };
    auto hitIdx = [&](int x, int y) { return size_t(y - region.y) * size_t(region.w) + size_t(x - region.x); };

    Rect touched{};
    auto note = [&](int x, int y) {
        touched = touched.isEmpty() ? Rect{x, y, 1, 1}
                                    : uniteRect(touched, Rect{x, y, 1, 1});
    };

    if (params.contiguous) {
        std::vector<std::pair<int, int>> stack;
        stack.emplace_back(sx, sy);
        while (!stack.empty()) {
            const auto [px, py] = stack.back();
            stack.pop_back();
            if (hit[hitIdx(px, py)] || !matches(px, py))
                continue;
            int xa = px;
            while (xa - 1 >= region.x && !hit[hitIdx(xa - 1, py)] && matches(xa - 1, py))
                --xa;
            int xb = px;
            while (xb + 1 < region.x + region.w && !hit[hitIdx(xb + 1, py)] && matches(xb + 1, py))
                ++xb;
            for (int x = xa; x <= xb; ++x)
                hit[hitIdx(x, py)] = 1;
            note(xa, py);
            note(xb, py);
            for (int ny : {py - 1, py + 1}) {
                if (ny < region.y || ny >= region.y + region.h)
                    continue;
                for (int x = xa; x <= xb; ++x) {
                    if (!hit[hitIdx(x, ny)] && matches(x, ny) &&
                        (x == xa || !matches(x - 1, ny) || hit[hitIdx(x - 1, ny)]))
                        stack.emplace_back(x, ny);
                }
            }
        }
    } else {
        for (int y = 0; y < region.h; ++y)
            for (int x = 0; x < region.w; ++x) {
                if (!matches(region.x + x, region.y + y))
                    continue;
                hit[size_t(y) * size_t(region.w) + size_t(x)] = 1;
            }
        if (!hit.empty()) {
            // touched bbox in one pass
            int x0 = region.w, y0 = region.h, x1 = -1, y1 = -1;
            for (int y = 0; y < region.h; ++y)
                for (int x = 0; x < region.w; ++x)
                    if (hit[size_t(y) * size_t(region.w) + size_t(x)]) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
            if (x1 >= 0)
                touched = Rect{region.x + x0, region.y + y0, x1 - x0 + 1, y1 - y0 + 1};
        }
    }
    if (touched.isEmpty())
        return {};

    // Rows are scanned top-down so the run list is sorted by (y, x0); the
    // writer below walks it with a monotonic cursor (tiles are visited in
    // row-major order).
    std::vector<Run> runs;
    for (int y = touched.y; y < touched.y + touched.h; ++y) {
        int x = touched.x;
        const int xEnd = touched.x + touched.w;
        while (x < xEnd) {
            const int gy = y, gx = x;
            if (!hit[hitIdx(gx, gy)]) {
                ++x;
                continue;
            }
            const int xs = x;
            while (x < xEnd && hit[hitIdx(x, gy)])
                ++x;
            runs.push_back({gy, xs, x});
        }
    }
    if (runs.empty())
        return {};

    size_t cursor = 0;
    target.store().updateTilesInRect(touched, memento, [&](TileData &t, int ox, int oy, const Rect &c) {
        while (cursor < runs.size() && runs[cursor].y < c.y)
            ++cursor;
        for (size_t i = cursor; i < runs.size() && runs[i].y < c.y + c.h; ++i) {
            const Run &run = runs[i];
            const int xa = std::max(run.x0, c.x);
            const int xb = std::min(run.x1, c.x + c.w);
            PixelRGBA8 *row = &t.px[(run.y - oy) * kTileSize + (xa - ox)];
            for (int x = xa; x < xb; ++x, ++row) {
                float cov = 1.0f;
                if (params.clip) {
                    cov = float(params.clip->coverage(x, run.y)) / 255.0f;
                    if (cov <= 0.002f)
                        continue;
                }
                PixelRGBA8 s = color;
                s.a = uint8_t(std::clamp(float(color.a) * cov, 0.0f, 255.0f));
                if (s.a == 0)
                    continue;
                *row = PaintDevice::compositeOver(*row, s);
            }
        }
    });
    return touched;
}

} // namespace ink
