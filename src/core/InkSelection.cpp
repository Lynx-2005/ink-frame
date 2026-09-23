#include "core/InkSelection.h"
#include <algorithm>
#include <cmath>

namespace ink {

void Selection::reset(int width, int height) {
    w_ = std::max(0, width);
    h_ = std::max(0, height);
    mask_.assign(size_t(w_) * size_t(h_), 0);
    bounds_ = Rect{};
    empty_ = true;
    outlineDirty_ = true;
    outline_.clear();
}

void Selection::clear() {
    std::fill(mask_.begin(), mask_.end(), uint8_t(0));
    bounds_ = Rect{};
    empty_ = true;
    outlineDirty_ = true;
}

void Selection::selectAll() {
    if (mask_.empty()) {
        empty_ = true;
        return;
    }
    std::fill(mask_.begin(), mask_.end(), uint8_t(255));
    bounds_ = Rect{0, 0, w_, h_};
    empty_ = false;
    outlineDirty_ = true;
}

void Selection::invert() {
    if (mask_.empty())
        return;
    for (uint8_t &v : mask_)
        v = uint8_t(255 - v);
    recomputeBounds();
    outlineDirty_ = true;
}

void Selection::translate(int dx, int dy) {
    if (empty_ || (dx == 0 && dy == 0))
        return;
    std::vector<uint8_t> shifted(mask_.size(), 0);
    for (int y = 0; y < h_; ++y) {
        const int ny = y + dy;
        if (ny < 0 || ny >= h_)
            continue;
        for (int x = 0; x < w_; ++x) {
            const uint8_t c = mask_[size_t(y) * size_t(w_) + size_t(x)];
            if (c == 0)
                continue;
            const int nx = x + dx;
            if (nx < 0 || nx >= w_)
                continue;
            shifted[size_t(ny) * size_t(w_) + size_t(nx)] = c;
        }
    }
    mask_.swap(shifted);
    bounds_ = intersectRect({bounds_.x + dx, bounds_.y + dy, bounds_.w, bounds_.h}, {0, 0, w_, h_});
    if (!outlineDirty_) {
        for (auto &seg : outline_) {
            seg[0] += float(dx);
            seg[1] += float(dy);
            seg[2] += float(dx);
            seg[3] += float(dy);
        }
    }
}

void Selection::mirror(bool horizontal) {
    if (empty_ || bounds_.isEmpty())
        return;
    auto at = [&](int x, int y) -> uint8_t & {
        return mask_[size_t(y) * size_t(w_) + size_t(x)];
    };
    if (horizontal) {
        for (int y = bounds_.y; y < bounds_.y + bounds_.h; ++y)
            for (int i = 0; i < bounds_.w / 2; ++i)
                std::swap(at(bounds_.x + i, y), at(bounds_.x + bounds_.w - 1 - i, y));
    } else {
        for (int x = bounds_.x; x < bounds_.x + bounds_.w; ++x)
            for (int i = 0; i < bounds_.h / 2; ++i)
                std::swap(at(x, bounds_.y + i), at(x, bounds_.y + bounds_.h - 1 - i));
    }
    outlineDirty_ = true;
}

void Selection::recomputeBounds() {
    int x0 = w_, y0 = h_, x1 = -1, y1 = -1;
    for (int y = 0; y < h_; ++y) {
        const uint8_t *row = &mask_[size_t(y) * size_t(w_)];
        for (int x = 0; x < w_; ++x) {
            if (row[x] == 0)
                continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    if (x1 < 0) {
        bounds_ = Rect{};
        empty_ = true;
    } else {
        bounds_ = Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        empty_ = false;
    }
}

// Scanline polygon rasterizer with 4 sub-scanlines and fractional x coverage.
void Selection::rasterizePolygon(const std::vector<Vec2> &pts, int w, int h, bool antialias,
                                 std::vector<uint8_t> &out, Rect &outBounds) {
    out.assign(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0);
    outBounds = Rect{};
    if (pts.size() < 3 || w <= 0 || h <= 0)
        return;

    double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (const Vec2 &p : pts) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    const int y0 = std::clamp(int(std::floor(minY)), 0, h);
    const int y1 = std::clamp(int(std::ceil(maxY)), 0, h);
    const int xLo = std::clamp(int(std::floor(minX)), 0, w);
    const int xHi = std::clamp(int(std::ceil(maxX)), 0, w);
    if (y1 <= y0 || xHi <= xLo)
        return;

    const int sub = antialias ? 4 : 1;
    std::vector<uint16_t> acc(size_t(xHi - xLo), 0);
    std::vector<std::pair<double, int>> cuts; // x, edge direction
    for (int y = y0; y < y1; ++y) {
        std::fill(acc.begin(), acc.end(), uint16_t(0));
        for (int s = 0; s < sub; ++s) {
            const double sy = y + (s + 0.5) / sub;
            cuts.clear();
            const size_t n = pts.size();
            for (size_t i = 0; i < n; ++i) {
                const Vec2 &a = pts[i];
                const Vec2 &b = pts[(i + 1) % n];
                if (a.y <= sy && b.y > sy) {
                    const double t = (sy - a.y) / (b.y - a.y);
                    cuts.emplace_back(a.x + t * (b.x - a.x), 1);
                } else if (b.y <= sy && a.y > sy) {
                    const double t = (sy - a.y) / (b.y - a.y);
                    cuts.emplace_back(a.x + t * (b.x - a.x), -1);
                }
            }
            if (cuts.size() < 2)
                continue;
            std::sort(cuts.begin(), cuts.end(),
                      [](const auto &l, const auto &r) { return l.first < r.first; });
            // Non-zero winding: a freehand lasso that crosses itself still
            // selects everything it swept (even-odd would punch holes).
            int winding = 0;
            double spanStart = 0.0;
            for (const auto &cut : cuts) {
                if (winding == 0)
                    spanStart = cut.first;
                winding += cut.second;
                if (winding != 0)
                    continue;
                const double xa = spanStart, xb = cut.first;
                if (xb <= xLo || xa >= xHi || xb <= xa)
                    continue;
                const int px0 = std::max(xLo, int(std::floor(xa)));
                const int px1 = std::min(xHi - 1, int(std::ceil(xb)) - 1);
                for (int x = px0; x <= px1; ++x) {
                    const double l = std::max(xa, double(x));
                    const double r = std::min(xb, double(x + 1));
                    if (r <= l)
                        continue;
                    acc[size_t(x - xLo)] += uint16_t(std::lround((r - l) * 255.0));
                }
            }
        }
        uint8_t *dstRow = &out[size_t(y) * size_t(w)];
        int rowX0 = w, rowX1 = -1;
        for (int x = xLo; x < xHi; ++x) {
            const int cov = int(acc[size_t(x - xLo)]) / sub;
            if (cov <= 0)
                continue;
            dstRow[x] = uint8_t(std::min(255, cov));
            rowX0 = std::min(rowX0, x);
            rowX1 = std::max(rowX1, x);
        }
        if (rowX1 >= 0) {
            if (outBounds.isEmpty()) {
                outBounds = Rect{rowX0, y, rowX1 - rowX0 + 1, 1};
            } else {
                const int bx1 = std::max(outBounds.x + outBounds.w - 1, rowX1);
                outBounds.x = std::min(outBounds.x, rowX0);
                outBounds.w = bx1 - outBounds.x + 1;
                outBounds.h = y - outBounds.y + 1;
            }
        }
    }
}

void Selection::applyPolygon(const std::vector<Vec2> &pts, SelectionAction action, bool antialias) {
    if (mask_.empty())
        return;
    std::vector<uint8_t> poly;
    Rect polyBounds;
    rasterizePolygon(pts, w_, h_, antialias, poly, polyBounds);
    if (polyBounds.isEmpty()) {
        if (action == SelectionAction::Replace)
            clear();
        return;
    }
    combineMask(poly, action);
    recomputeBounds();
    outlineDirty_ = true;
}

void Selection::combineMask(const std::vector<uint8_t> &poly, SelectionAction action) {
    // Only touch the rows the polygon covers: a full-canvas pass would be
    // wasteful for a small lasso.
    int y0 = h_, y1 = -1;
    for (int y = 0; y < h_; ++y) {
        const uint8_t *row = &poly[size_t(y) * size_t(w_)];
        bool any = false;
        for (int x = 0; x < w_ && !any; ++x)
            any = row[x] != 0;
        if (any) {
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    if (y1 < 0)
        return;
    for (int y = y0; y <= y1; ++y) {
        const uint8_t *src = &poly[size_t(y) * size_t(w_)];
        uint8_t *dst = &mask_[size_t(y) * size_t(w_)];
        switch (action) {
        case SelectionAction::Replace:
            std::copy(src, src + w_, dst);
            break;
        case SelectionAction::Add:
            for (int x = 0; x < w_; ++x)
                dst[x] = std::max(dst[x], src[x]);
            break;
        case SelectionAction::Subtract:
            for (int x = 0; x < w_; ++x)
                dst[x] = uint8_t(int(dst[x]) * (255 - int(src[x])) / 255);
            break;
        case SelectionAction::Intersect:
            for (int x = 0; x < w_; ++x)
                dst[x] = uint8_t(int(dst[x]) * int(src[x]) / 255);
            break;
        }
    }
}

const std::vector<std::array<float, 4>> &Selection::outline() {
    if (!outlineDirty_)
        return outline_;
    outlineDirty_ = false;
    outline_.clear();
    if (empty_ || bounds_.isEmpty())
        return outline_;

    const Rect r = intersectRect({bounds_.x - 1, bounds_.y - 1, bounds_.w + 2, bounds_.h + 2},
                                 Rect{0, 0, w_, h_});
    auto sel = [&](int x, int y) { return coverage(x, y) > 127; };

    // Only pixels that actually sit on the boundary may contribute a segment:
    // emitting a whole row/column span for a sloped edge would draw long lines
    // across the shape (the classic cross-hatched marching ants).
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int pass = 0; pass < 2; ++pass) {
            const int ny = y + (pass == 0 ? -1 : 1);
            const float ey = float(pass == 0 ? y : y + 1);
            int x = r.x;
            while (x < r.x + r.w) {
                if (!(sel(x, y) && !sel(x, ny))) {
                    ++x;
                    continue;
                }
                const int xs = x;
                while (x < r.x + r.w && sel(x, y) && !sel(x, ny))
                    ++x;
                outline_.push_back({float(xs), ey, float(x), ey});
            }
        }
    }
    for (int x = r.x; x < r.x + r.w; ++x) {
        for (int pass = 0; pass < 2; ++pass) {
            const int nx = x + (pass == 0 ? -1 : 1);
            const float ex = float(pass == 0 ? x : x + 1);
            int y = r.y;
            while (y < r.y + r.h) {
                if (!(sel(x, y) && !sel(nx, y))) {
                    ++y;
                    continue;
                }
                const int ys = y;
                while (y < r.y + r.h && sel(x, y) && !sel(nx, y))
                    ++y;
                outline_.push_back({ex, float(ys), ex, float(y)});
            }
        }
    }
    return outline_;
}

} // namespace ink
