#include "core/InkSelection.h"
#include "core/InkPaintDevice.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ink {

void Selection::reset(int width, int height) {
    w_ = std::max(0, width);
    h_ = std::max(0, height);
    mask_.assign(size_t(w_) * size_t(h_), 0);
    bounds_ = Rect{};
    empty_ = true;
    display_ = Mat3::identity();
    displayInv_ = Mat3::identity();
    displayActive_ = false;
    outlineDirty_ = true;
    outline_.clear();
}

void Selection::clear() {
    std::fill(mask_.begin(), mask_.end(), uint8_t(0));
    bounds_ = Rect{};
    empty_ = true;
    display_ = Mat3::identity();
    displayInv_ = Mat3::identity();
    displayActive_ = false;
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
    // The cached runs are in DISPLAY space, so the cheap device-space shift
    // below is only right when nothing displaces the ink; otherwise rebuild.
    if (displayActive_)
        outlineDirty_ = true;
    else if (!outlineDirty_) {
        for (auto &seg : outline_) {
            seg[0] += float(dx);
            seg[1] += float(dy);
            seg[2] += float(dx);
            seg[3] += float(dy);
        }
    }
}

void Selection::resampleMask(const Mat3 &forward, Interpolation filter) {
    if (w_ <= 0 || h_ <= 0 || mask_.empty() || empty_)
        return;
    std::vector<uint8_t> out(size_t(w_) * size_t(h_), 0);
    resampleCoverage(mask_.data(), w_, h_, {0, 0, w_, h_}, forward, filter, out.data());
    mask_.swap(out);
    recomputeBounds();
    outlineDirty_ = true;
}

void Selection::resampleMask(const DeformMesh &mesh) {
    if (w_ <= 0 || h_ <= 0 || mask_.empty() || empty_ || !mesh.valid())
        return;
    std::vector<uint8_t> out(size_t(w_) * size_t(h_), 0);
    warpCoverage(mask_.data(), w_, h_, {0, 0, w_, h_}, mesh, out.data());
    mask_.swap(out);
    recomputeBounds();
    outlineDirty_ = true;
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
            // Binary cut at the ants threshold: everything above 127 stores 255,
            // everything below stays 0. Keeping a raw AA fringe would store
            // half-covered edge pixels that `contains()` (coverage > 127) and
            // liftRegion treat as OUTSIDE while the marching ants draw over
            // them — the selection reads as faded, and a lift keeps a
            // half-transparent fringe of edge ink.
            dstRow[x] = cov > 127 ? uint8_t(255) : uint8_t(0);
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

void Selection::applyPolygon(const std::vector<Vec2> &ptsIn, SelectionAction action, bool antialias) {
    if (mask_.empty())
        return;
    // The lasso is drawn in DOCUMENT space; the mask lives in DEVICE space.
    // With the animation displacing the ink, the polygon is back-mapped so it
    // lands on the pixels that produce what the user circled — then the fresh
    // mask keeps following the animation.
    std::vector<Vec2> pts = ptsIn;
    if (displayActive_)
        for (Vec2 &p : pts)
            p = displayInv_.map(p);
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

void Selection::applyStrokePolygon(const std::vector<Vec2> &ptsIn, SelectionAction action,
                                   bool antialias, const PaintDevice &device, int gapRadius) {
    if (mask_.empty())
        return;
    // Same document->device mapping as applyPolygon: the grab must find the
    // ink where it DISPLAYS, then store the mask where the pixels ARE.
    std::vector<Vec2> pts = ptsIn;
    if (displayActive_)
        for (Vec2 &p : pts)
            p = displayInv_.map(p);
    std::vector<uint8_t> poly;
    Rect polyBounds;
    rasterizePolygon(pts, w_, h_, antialias, poly, polyBounds);
    if (polyBounds.isEmpty()) {
        if (action == SelectionAction::Replace)
            clear();
        return;
    }
    const int r = std::clamp(gapRadius, 0, 32);
    // Dilated touch mask: square dilation via integral image so a rough loop
    // around the drawing still counts as touching the stroke.
    std::vector<uint8_t> touch(size_t(w_) * size_t(h_), 0);
    if (r <= 0) {
        touch = poly;
    } else {
        std::vector<int> integral(size_t(w_ + 1) * size_t(h_ + 1), 0);
        const int stride = w_ + 1;
        for (int y = 0; y < h_; ++y) {
            int rowSum = 0;
            for (int x = 0; x < w_; ++x) {
                rowSum += poly[size_t(y) * size_t(w_) + size_t(x)] ? 1 : 0;
                integral[size_t(y + 1) * size_t(stride) + x + 1] =
                    integral[size_t(y) * size_t(stride) + x + 1] + rowSum;
            }
        }
        auto rectSum = [&](int x0, int y0, int x1, int y1) {
            // inclusive bounds, clamped
            x0 = std::clamp(x0, 0, w_ - 1);
            y0 = std::clamp(y0, 0, h_ - 1);
            x1 = std::clamp(x1, 0, w_ - 1);
            y1 = std::clamp(y1, 0, h_ - 1);
            if (x1 < x0 || y1 < y0) return 0;
            const int A = integral[size_t(y0) * size_t(stride) + x0];
            const int B = integral[size_t(y0) * size_t(stride) + x1 + 1];
            const int C = integral[size_t(y1 + 1) * size_t(stride) + x0];
            const int D = integral[size_t(y1 + 1) * size_t(stride) + x1 + 1];
            return D - B - C + A;
        };
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                touch[size_t(y) * size_t(w_) + size_t(x)] =
                    rectSum(x - r, y - r, x + r, y + r) > 0 ? uint8_t(255) : uint8_t(0);
    }

    // Ink mask of the current cell (alpha threshold), clipped to its extent.
    const Rect ext = intersectRect(device.extent(), {0, 0, w_, h_});
    if (ext.isEmpty()) {
        // Nothing drawn: plain polygon behaviour.
        combineMask(poly, action);
        recomputeBounds();
        outlineDirty_ = true;
        return;
    }
    std::vector<uint8_t> rgba(size_t(ext.w) * size_t(ext.h) * 4);
    device.readRect(ext, rgba.data());
    const int ew = ext.w, eh = ext.h;
    std::vector<uint8_t> ink(size_t(ew) * size_t(eh), 0);
    for (int y = 0; y < eh; ++y)
        for (int x = 0; x < ew; ++x)
            ink[size_t(y) * size_t(ew) + size_t(x)] =
                rgba[(size_t(y) * size_t(ew) + size_t(x)) * 4 + 3] > 10 ? uint8_t(1) : uint8_t(0);

    // Seeds: ink pixels touched by the (dilated) lasso.
    std::vector<int> stack;
    stack.reserve(4096);
    std::vector<uint8_t> seen(size_t(ew) * size_t(eh), 0);
    for (int y = 0; y < eh; ++y) {
        for (int x = 0; x < ew; ++x) {
            if (!ink[size_t(y) * size_t(ew) + size_t(x)]) continue;
            const int gx = ext.x + x, gy = ext.y + y;
            if (touch[size_t(gy) * size_t(w_) + size_t(gx)] == 0) continue;
            seen[size_t(y) * size_t(ew) + size_t(x)] = 1;
            stack.push_back(y * ew + x);
        }
    }
    if (stack.empty()) {
        // Lasso touched no ink: plain polygon behaviour (selects the area).
        combineMask(poly, action);
        recomputeBounds();
        outlineDirty_ = true;
        return;
    }
    // 8-connected flood from all seeds: the union is the set of whole strokes.
    std::vector<uint8_t> stroke(size_t(w_) * size_t(h_), 0);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % ew, y = i / ew;
        stroke[size_t(ext.y + y) * size_t(w_) + size_t(ext.x + x)] = 255;
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= eh) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx;
                if (nx < 0 || nx >= ew) continue;
                const size_t ni = size_t(ny) * size_t(ew) + size_t(nx);
                if (seen[ni] || !ink[ni]) continue;
                seen[ni] = 1;
                stack.push_back(int(ni));
            }
        }
    }
    combineMask(stroke, action);
    recomputeBounds();
    outlineDirty_ = true;
}

void Selection::setDocumentMatrix(const Mat3 &m) {
    // A singular map (a scale dragged to zero) would make the back-mapping
    // meaningless; hold the last good one rather than breaking hit-testing.
    bool ok = false;
    const Mat3 inv = m.inverted(&ok);
    if (!ok)
        return;
    const bool active = !m.isIdentity();
    if (active == displayActive_ && m.m[0] == display_.m[0] && m.m[1] == display_.m[1] &&
        m.m[2] == display_.m[2] && m.m[3] == display_.m[3] && m.m[4] == display_.m[4] &&
        m.m[5] == display_.m[5] && m.m[6] == display_.m[6] && m.m[7] == display_.m[7] &&
        m.m[8] == display_.m[8])
        return;
    display_ = m;
    displayInv_ = inv;
    displayActive_ = active;
    outlineDirty_ = true;
}

Rect Selection::docBounds() const {
    if (empty_)
        return {};
    if (!displayActive_)
        return bounds_;
    Vec2 c[4];
    rectCorners(bounds_, c);
    for (Vec2 &p : c)
        p = display_.map(p);
    return cornersBounds(c);
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

    // The outline is drawn in DOCUMENT space: with the animation displacing the
    // ink, the scanned runs are pushed through the display MATRIX — not a
    // translation, which would leave the ants beside a rotated stroke instead
    // of on it. Mapping the segments (rather than re-rasterizing the mask)
    // keeps this cheap, and the mask itself stays in device space for the
    // pixel pipelines.
    auto push = [&](float x0, float y0, float x1, float y1) {
        if (displayActive_) {
            const Vec2 a = display_.map({x0, y0});
            const Vec2 b = display_.map({x1, y1});
            outline_.push_back({float(a.x), float(a.y), float(b.x), float(b.y)});
        } else {
            outline_.push_back({x0, y0, x1, y1});
        }
    };

    // Scan the mask in its OWN pixel space (coverageRaw), then emit runs at
    // the animated position. (coverage() already folds the offset in, but the
    // scan must walk the raw mask bounds, not the shifted ones.)
    const Rect r = intersectRect({bounds_.x - 1, bounds_.y - 1, bounds_.w + 2, bounds_.h + 2},
                                 Rect{0, 0, w_, h_});
    auto sel = [&](int x, int y) { return coverageRaw(x, y) > 127; };

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
                push(float(xs), ey, float(x), ey);
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
                push(ex, float(ys), ex, float(y));
            }
        }
    }
    return outline_;
}

} // namespace ink
