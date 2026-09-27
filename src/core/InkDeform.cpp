#include "core/InkDeform.h"
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

// Weighted Moving Least Squares, sharing one scratch buffer across calls (the
// warp evaluates this once per lattice node, so a per-call allocation would
// dominate). The three anchor strengths differ only in which transform is
// fitted around each control point:
//   Affine      - full 2x2 least-squares fit (most flexible)
//   Similitude  - rotation + uniform scale (Procrustes, in complex form)
//   Rigid       - rotation only, the strongest anchor
Vec2 mlsWithScratch(Vec2 v, const std::vector<Vec2> &p, const std::vector<Vec2> &q, double alpha,
                    MlsKind kind, std::vector<double> &w) {
    const size_t n = std::min(p.size(), q.size());
    if (n == 0)
        return v;
    if (n == 1)
        return {v.x + (q[0].x - p[0].x), v.y + (q[0].y - p[0].y)}; // just a translation

    w.resize(n);
    double sw = 0.0, psx = 0.0, psy = 0.0, qsx = 0.0, qsy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::hypot(p[i].x - v.x, p[i].y - v.y);
        d = std::max(d, 1e-3); // a control point sitting on v dominates, never divides by zero
        const double wi = 1.0 / std::pow(d, 2.0 * alpha);
        w[i] = wi;
        sw += wi;
        psx += wi * p[i].x;
        psy += wi * p[i].y;
        qsx += wi * q[i].x;
        qsy += wi * q[i].y;
    }
    if (!(sw > 0.0))
        return v;
    const Vec2 pStar{psx / sw, psy / sw};
    const Vec2 qStar{qsx / sw, qsy / sw};

    // Normalised sums: A = sum w pHat pHat^T, E = sum w qHat pHat^T.
    double a = 0.0, b = 0.0, c = 0.0; // A
    double e = 0.0, f = 0.0, g = 0.0, h = 0.0; // E
    for (size_t i = 0; i < n; ++i) {
        const double px = p[i].x - pStar.x, py = p[i].y - pStar.y;
        const double qx = q[i].x - qStar.x, qy = q[i].y - qStar.y;
        const double wi = w[i];
        a += wi * px * px;
        b += wi * px * py;
        c += wi * py * py;
        e += wi * qx * px;
        f += wi * qx * py;
        g += wi * qy * px;
        h += wi * qy * py;
    }

    double m00 = 1.0, m01 = 0.0, m10 = 0.0, m11 = 1.0;
    bool fitted = false;
    if (kind == MlsKind::Affine) {
        const double det = a * c - b * b;
        if (std::abs(det) > 1e-12) {
            m00 = (e * c - f * b) / det;
            m01 = (f * a - e * b) / det;
            m10 = (g * c - h * b) / det;
            m11 = (h * a - g * b) / det;
            fitted = true;
        }
    } else {
        // Weighted Procrustes in complex form: lambda = sum w conj(pHat) qHat /
        // sum w |pHat|^2. Its real part is s = sum w (pHat . qHat) and its
        // imaginary part is t = sum w (pHat x qHat); M is that complex number's
        // matrix [[s, -t], [t, s]] (column convention, matching q = M p).
        const double s = e + h;
        const double t = g - f;
        const double norm = std::hypot(s, t);
        // Similarity keeps the fitted scale; Rigid is rotation only, which is
        // what makes it the strongest anchor.
        const double den = (kind == MlsKind::Rigid) ? norm : (a + c);
        if (den > 1e-12) {
            m00 = s / den;
            m01 = -t / den;
            m10 = t / den;
            m11 = s / den;
            fitted = true;
        }
    }
    if (!fitted) {
        // Degenerate control set: the limit is a plain translation.
        return {v.x + (qStar.x - pStar.x), v.y + (qStar.y - pStar.y)};
    }
    const Vec2 r{v.x - pStar.x, v.y - pStar.y};
    return {m00 * r.x + m01 * r.y + qStar.x, m10 * r.x + m11 * r.y + qStar.y};
}

} // namespace

Vec2 mlsTransform(Vec2 v, const std::vector<Vec2> &p, const std::vector<Vec2> &q, double alpha,
                  MlsKind kind) {
    std::vector<double> scratch;
    return mlsWithScratch(v, p, q, alpha, kind, scratch);
}

// --- lattice ----------------------------------------------------------------

DeformMesh DeformMesh::make(const Rect &bounds, int cols, int rows) {
    DeformMesh m;
    m.bounds = bounds;
    m.cols = std::max(1, cols);
    m.rows = std::max(1, rows);
    const int nx = m.cols + 1;
    m.orig.resize(size_t(nx) * size_t(m.rows + 1));
    for (int j = 0; j <= m.rows; ++j) {
        for (int i = 0; i <= m.cols; ++i) {
            const double x = double(bounds.x) + double(bounds.w) * double(i) / double(m.cols);
            const double y = double(bounds.y) + double(bounds.h) * double(j) / double(m.rows);
            m.orig[size_t(j) * size_t(nx) + size_t(i)] = {x, y};
        }
    }
    m.moved = m.orig;
    return m;
}

Vec2 DeformMesh::node(int cx, int cy) const {
    return orig[size_t(cy) * size_t(cols + 1) + size_t(cx)];
}

Vec2 DeformMesh::movedNode(int cx, int cy) const {
    return moved[size_t(cy) * size_t(cols + 1) + size_t(cx)];
}

void DeformMesh::deformByPoints(const std::vector<Vec2> &p, const std::vector<Vec2> &q,
                                double alpha, MlsKind kind) {
    if (!valid() || p.empty() || p.size() != q.size())
        return;
    std::vector<double> scratch;
    for (size_t k = 0; k < orig.size(); ++k)
        moved[k] = mlsWithScratch(orig[k], p, q, alpha, kind, scratch);
}

Vec2 DeformMesh::offsetAt(Vec2 dst) const {
    if (!valid() || bounds.w <= 0 || bounds.h <= 0)
        return {0.0, 0.0};
    if (dst.x < double(bounds.x) || dst.y < double(bounds.y) ||
        dst.x > double(bounds.x + bounds.w) || dst.y > double(bounds.y + bounds.h))
        return {0.0, 0.0}; // outside the lattice nothing is displaced
    const int nx = cols + 1;
    const double fx = (dst.x - double(bounds.x)) / double(bounds.w) * double(cols);
    const double fy = (dst.y - double(bounds.y)) / double(bounds.h) * double(rows);
    const int i0 = std::min(int(fx), cols - 1);
    const int j0 = std::min(int(fy), rows - 1);
    const double tx = fx - double(i0), ty = fy - double(j0);
    auto disp = [&](int i, int j) {
        const size_t k = size_t(j) * size_t(nx) + size_t(i);
        return Vec2{moved[k].x - orig[k].x, moved[k].y - orig[k].y};
    };
    const Vec2 d00 = disp(i0, j0), d10 = disp(i0 + 1, j0);
    const Vec2 d01 = disp(i0, j0 + 1), d11 = disp(i0 + 1, j0 + 1);
    const Vec2 top{d00.x + (d10.x - d00.x) * tx, d00.y + (d10.y - d00.y) * tx};
    const Vec2 bottom{d01.x + (d11.x - d01.x) * tx, d01.y + (d11.y - d01.y) * tx};
    return {top.x + (bottom.x - top.x) * ty, top.y + (bottom.y - top.y) * ty};
}

Vec2 DeformMesh::sourceOf(Vec2 dst) const {
    // Fixed-point inversion of the displacement field: for the deformations a
    // brush produces this converges in two steps and never divides by a
    // Jacobian that a fold could have made singular.
    Vec2 s = dst - offsetAt(dst);
    s = dst - offsetAt(s);
    s = dst - offsetAt(s);
    return s;
}

Rect DeformMesh::movedBounds() const {
    if (!valid())
        return {};
    double x0 = moved[0].x, x1 = x0, y0 = moved[0].y, y1 = y0;
    for (const Vec2 &p : moved) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    // Bilinear blending between nodes can overshoot the node hull by up to a
    // cell: pad so nothing gets clipped.
    const double pw = double(bounds.w) / double(cols), ph = double(bounds.h) / double(rows);
    const int ix0 = int(std::floor(x0 - pw)), iy0 = int(std::floor(y0 - ph));
    const int ix1 = int(std::ceil(x1 + pw)), iy1 = int(std::ceil(y1 + ph));
    return {ix0, iy0, ix1 - ix0, iy1 - iy0};
}

bool DeformMesh::isIdentity(double eps) const {
    if (!valid())
        return true;
    for (size_t i = 0; i < orig.size(); ++i) {
        if (std::abs(moved[i].x - orig[i].x) > eps || std::abs(moved[i].y - orig[i].y) > eps)
            return false;
    }
    return true;
}

FloatRegion warpRegion(const FloatRegion &src, const Rect &dstRect, const DeformMesh &mesh) {
    FloatRegion out;
    if (dstRect.isEmpty())
        return out;
    out.bounds = dstRect;
    out.px.assign(size_t(dstRect.w) * size_t(dstRect.h), PixelRGBA8{0, 0, 0, 0});
    if (src.empty() || !mesh.valid())
        return out;
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            PixelRGBA8 *row = &out.px[size_t(y) * size_t(dstRect.w)];
            const double dy = double(dstRect.y + y) + 0.5;
            for (int x = 0; x < dstRect.w; ++x) {
                const Vec2 dst{double(dstRect.x + x) + 0.5, dy};
                row[x] = sampleRegionBilinear(src, mesh.sourceOf(dst));
            }
        }
    };
    runRowBands(dstRect.h, out.px.size(), rows);
    return out;
}

void warpCoverage(const uint8_t *src, int sw, int sh, const Rect &dstRect, const DeformMesh &mesh,
                  uint8_t *dstOut) {
    if (!src || !dstOut || dstRect.isEmpty() || sw <= 0 || sh <= 0)
        return;
    std::fill(dstOut, dstOut + size_t(dstRect.w) * size_t(dstRect.h), uint8_t(0));
    if (!mesh.valid())
        return;
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint8_t *row = dstOut + size_t(y) * size_t(dstRect.w);
            const double dy = double(dstRect.y + y) + 0.5;
            for (int x = 0; x < dstRect.w; ++x) {
                const Vec2 s = mesh.sourceOf({double(dstRect.x + x) + 0.5, dy});
                const int sx = int(std::floor(s.x)), sy = int(std::floor(s.y));
                if (sx < 0 || sy < 0 || sx >= sw || sy >= sh)
                    continue;
                row[x] = src[size_t(sy) * size_t(sw) + size_t(sx)];
            }
        }
    };
    runRowBands(dstRect.h, size_t(dstRect.w) * size_t(dstRect.h), rows);
}

// --- liquify ----------------------------------------------------------------

void LiquifyField::reset(const Rect &bounds, int cellPx) {
    const int cell = std::max(1, cellPx);
    // Grid resolution follows a MINIMUM node count, not the region's pixel
    // size. The region is the lifted CONTENT bounds, which can be a small
    // stroke: at 12px cells a 40x30 dab area got a 3x2 lattice, too coarse for
    // the brush to move anything at all (the stroke looked dead). A floor of
    // 8 keeps the field responsive on small selections; the cost is trivial
    // (81 nodes) and invisible on large ones.
    const int cols = std::max(8, bounds.w / cell);
    const int rows = std::max(8, bounds.h / cell);
    mesh_ = DeformMesh::make(bounds, cols, rows);
}

template <typename Fn>
void LiquifyField::forEachNodeInRange(Vec2 center, double radius, Fn &&fn) {
    if (!mesh_.valid() || radius <= 0.0)
        return;
    const double sigma = std::max(1e-3, radius / 3.0);
    const double twoSigma2 = 2.0 * sigma * sigma;
    for (size_t k = 0; k < mesh_.orig.size(); ++k) {
        const Vec2 &o = mesh_.orig[k];
        const double dx = o.x - center.x, dy = o.y - center.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > radius * radius)
            continue;
        fn(k, std::exp(-d2 / twoSigma2));
    }
}

void LiquifyField::dab(Brush brush, Vec2 center, Vec2 delta, double factor, double angleRad,
                       const Params &params) {
    if (!mesh_.valid())
        return;
    const double amount = std::clamp(params.amount, 0.0, 1.0);
    const double flow = std::clamp(params.flow, 0.0, 1.0);
    forEachNodeInRange(center, params.radius, [&](size_t k, double falloff) {
        const Vec2 o = mesh_.orig[k];
        Vec2 &m = mesh_.moved[k];
        // Build-up applies the brush directly; Wash blends by flow and (for a
        // push) only where it increases the distance from the original.
        const double a = params.wash ? amount * flow * falloff : amount * falloff;
        if (a <= 0.0)
            return;
        switch (brush) {
        case Brush::Move: {
            const Vec2 target{m.x + delta.x * a, m.y + delta.y * a};
            if (params.wash) {
                const double was = std::hypot(m.x - o.x, m.y - o.y);
                const double now = std::hypot(target.x - o.x, target.y - o.y);
                if (now <= was)
                    return;
            }
            m = target;
            break;
        }
        case Brush::Scale: {
            const double f = 1.0 + (factor - 1.0) * a;
            m = {center.x + (m.x - center.x) * f, center.y + (m.y - center.y) * f};
            break;
        }
        case Brush::Rotate: {
            const double ang = angleRad * a;
            const double c = std::cos(ang), s = std::sin(ang);
            const double dx = m.x - center.x, dy = m.y - center.y;
            m = {center.x + c * dx - s * dy, center.y + s * dx + c * dy};
            break;
        }
        case Brush::Undo:
            m = {m.x + (o.x - m.x) * a, m.y + (o.y - m.y) * a};
            break;
        }
    });
}

} // namespace ink
