#include "core/InkTransform.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace ink {

// --- Mat3 -------------------------------------------------------------------

Mat3 Mat3::fromAffine(const Affine2D &a) {
    Mat3 r;
    r.m[0] = a.a; r.m[1] = a.c; r.m[2] = a.tx;
    r.m[3] = a.b; r.m[4] = a.d; r.m[5] = a.ty;
    return r;
}

Mat3 Mat3::translation(double dx, double dy) {
    Mat3 r;
    r.m[2] = dx;
    r.m[5] = dy;
    return r;
}

Mat3 Mat3::scale(double sx, double sy) {
    Mat3 r;
    r.m[0] = sx;
    r.m[4] = sy;
    return r;
}

Mat3 Mat3::rotation(double degrees) {
    const double rad = degrees * M_PI / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    Mat3 r;
    r.m[0] = c; r.m[1] = -s;
    r.m[3] = s; r.m[4] = c;
    return r;
}

Mat3 Mat3::shear(double kx, double ky) {
    Mat3 r;
    r.m[1] = kx;
    r.m[3] = ky;
    return r;
}

Vec2 Mat3::map(Vec2 p) const {
    const double w = m[6] * p.x + m[7] * p.y + m[8];
    if (!(std::abs(w) > 1e-12))
        return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    const double iw = 1.0 / w;
    return {(m[0] * p.x + m[1] * p.y + m[2]) * iw, (m[3] * p.x + m[4] * p.y + m[5]) * iw};
}

Mat3 Mat3::multiply(const Mat3 &o) const {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = m[i * 3] * o.m[j] + m[i * 3 + 1] * o.m[3 + j] + m[i * 3 + 2] * o.m[6 + j];
    return r;
}

Mat3 Mat3::inverted(bool *ok) const {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
                 i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::abs(det) > 1e-12)) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    const double id = 1.0 / det;
    Mat3 r;
    r.m[0] = A * id;
    r.m[1] = (c * h - b * i) * id;
    r.m[2] = (b * f - c * e) * id;
    r.m[3] = B * id;
    r.m[4] = (a * i - c * g) * id;
    r.m[5] = (c * d - a * f) * id;
    r.m[6] = C * id;
    r.m[7] = (b * g - a * h) * id;
    r.m[8] = (a * e - b * d) * id;
    return r;
}

bool Mat3::isIdentity(double eps) const {
    static const double id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i)
        if (std::abs(m[i] - id[i]) > eps)
            return false;
    return true;
}

// --- geometry ---------------------------------------------------------------

void rectCorners(const Rect &r, Vec2 out[4]) {
    const double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    out[0] = {x0, y0};
    out[1] = {x1, y0};
    out[2] = {x1, y1};
    out[3] = {x0, y1};
}

Rect cornersBounds(const Vec2 q[4]) {
    double x0 = q[0].x, x1 = q[0].x, y0 = q[0].y, y1 = q[0].y;
    for (int i = 1; i < 4; ++i) {
        x0 = std::min(x0, q[i].x);
        x1 = std::max(x1, q[i].x);
        y0 = std::min(y0, q[i].y);
        y1 = std::max(y1, q[i].y);
    }
    const int ix0 = int(std::floor(x0)), iy0 = int(std::floor(y0));
    const int ix1 = int(std::ceil(x1)), iy1 = int(std::ceil(y1));
    return {ix0, iy0, ix1 - ix0, iy1 - iy0};
}

namespace {

// Unit square -> quad, corner order 0=TL 1=TR 2=BR 3=BL (Heckbert's solve).
bool squareToQuad(const Vec2 q[4], Mat3 &m) {
    const double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x;
    const double dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
    const double sx = q[0].x - q[1].x + q[2].x - q[3].x;
    const double sy = q[0].y - q[1].y + q[2].y - q[3].y;
    double g = 0.0, h = 0.0;
    if (std::abs(sx) > 1e-12 || std::abs(sy) > 1e-12) {
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::abs(den) < 1e-12)
            return false;
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
    }
    m.m[0] = q[1].x - q[0].x + g * q[1].x;
    m.m[1] = q[3].x - q[0].x + h * q[3].x;
    m.m[2] = q[0].x;
    m.m[3] = q[1].y - q[0].y + g * q[1].y;
    m.m[4] = q[3].y - q[0].y + h * q[3].y;
    m.m[5] = q[0].y;
    m.m[6] = g;
    m.m[7] = h;
    m.m[8] = 1.0;
    return true;
}

} // namespace

bool quadToQuad(const Vec2 from[4], const Vec2 to[4], Mat3 &out) {
    Mat3 a, b;
    if (!squareToQuad(from, a) || !squareToQuad(to, b))
        return false;
    bool ok = false;
    const Mat3 invA = a.inverted(&ok);
    if (!ok)
        return false;
    out = b.multiply(invA);
    // A degenerate destination quad (collapsed corners) yields a singular map:
    // refuse it rather than hand out a matrix that cannot be inverted.
    bool invertible = false;
    out.inverted(&invertible);
    return invertible;
}

// --- free transform parameters ----------------------------------------------

void FreeTransformParams::reset() {
    // Explicit field reset rather than `*this = {}`: an aggregate assignment
    // with an array member trips a GCC 13 ICE (gimple_add_tmp_var).
    translate = {0.0, 0.0};
    scaleX = scaleY = 1.0;
    angleDeg = shearX = shearY = 0.0;
    hasQuad = false;
    for (Vec2 &c : quadSrc)
        c = {0.0, 0.0};
}

Mat3 FreeTransformParams::matrix(const Rect &sourceRect) const {
    Mat3 m = Mat3::translation(translate.x, translate.y)
                 .multiply(Mat3::translation(pivot.x, pivot.y))
                 .multiply(Mat3::rotation(angleDeg))
                 .multiply(Mat3::shear(shearX, shearY))
                 .multiply(Mat3::scale(scaleX, scaleY))
                 .multiply(Mat3::translation(-pivot.x, -pivot.y));
    if (hasQuad) {
        Vec2 from[4];
        rectCorners(sourceRect, from);
        Mat3 q;
        if (quadToQuad(from, quadSrc, q))
            m = m.multiply(q); // the override acts on the source, before the rest
    }
    return m;
}

// --- 1-D filter kernels -----------------------------------------------------

namespace {

inline constexpr double kInv255 = 1.0 / 255.0;

inline uint8_t quantize255(double v) { return uint8_t(std::clamp(v, 0.0, 255.0) + 0.5); }

// std::floor is a libm call at the baseline x86-64 ISA, and the sampler needs
// one per axis per pixel: truncation + a compare is exact here and free.
inline int floorToInt(double v) {
    const int i = int(v);
    return v < double(i) ? i - 1 : i;
}

double sinc(double x) {
    if (std::abs(x) < 1e-9)
        return 1.0;
    const double p = M_PI * x;
    return std::sin(p) / p;
}

// Interpolating cubic (Catmull-Rom): exact at 0, zero at ±1, ±2.
double catmullRom(double x) {
    x = std::abs(x);
    if (x < 1.0)
        return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0)
        return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

double lanczos3(double x) {
    x = std::abs(x);
    return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
}

// The filter is a compile-time parameter: with the tap count known, the kernel
// collapses into straight-line code and the whole sampler inlines. A runtime
// switch in the per-pixel path made a 1.5 Mpx frame ~3x slower (the axis-tap
// helper ended up as an out-of-line call returning a 56-byte struct).
template <Interpolation F>
struct Kernel {
    static constexpr int taps = F == Interpolation::Nearest   ? 1
                                : F == Interpolation::Bilinear ? 2
                                : F == Interpolation::Bicubic  ? 4
                                                               : 6;
    static constexpr int offset = F == Interpolation::Nearest    ? 0
                                  : F == Interpolation::Bilinear ? 0
                                  : F == Interpolation::Bicubic  ? -1
                                                                 : -2;

    // u = source position in pixel-index space (pixel i centred at i).
    // Every kernel interpolates exactly at integer u, so an integer-aligned
    // map (translation, 90-degree step) reproduces the source bit for bit.
    // `first` gets the `taps` consecutive source pixels, `w` their weights.
    static inline void weights(double u, int *first, double *w) {
        int base;
        if constexpr (F == Interpolation::Nearest) {
            base = floorToInt(u + 0.5);
            w[0] = 1.0;
        } else {
            const int i0 = floorToInt(u);
            const double f = u - double(i0);
            base = i0 + offset;
            if constexpr (F == Interpolation::Bilinear) {
                w[0] = 1.0 - f;
                w[1] = f;
            } else if constexpr (F == Interpolation::Bicubic) {
                w[0] = catmullRom(f + 1.0);
                w[1] = catmullRom(f);
                w[2] = catmullRom(1.0 - f);
                w[3] = catmullRom(2.0 - f);
            } else {
                for (int k = 0; k < taps; ++k)
                    w[k] = lanczos3(f - double(k - 2));
            }
        }
        for (int k = 0; k < taps; ++k)
            first[k] = base + k;
    }
};

// Weighted sum of the (NX x NY) tap neighbourhood. `fetch` accumulates
// premultiplied samples; taps outside the source simply contribute nothing.
template <int NX, int NY, typename Fetch>
inline void accumulateTaps(const int *xf, const double *wx, const int *yf, const double *wy,
                           double *acc, Fetch &fetch) {
    for (int j = 0; j < NY; ++j) {
        const double wyj = wy[j];
        if (wyj == 0.0)
            continue;
        for (int i = 0; i < NX; ++i) {
            const double w = wx[i] * wyj;
            if (w == 0.0)
                continue;
            fetch(xf[i], yf[j], w, acc);
        }
    }
}

} // namespace

// Row-band parallelism (declared in the header so the deformation core can use
// it too): the sampler is a pure function of (destination row, immutable
// source), so bands are independent and produce bit-identical output to a
// single-threaded run. Preview frames are the interactive bottleneck — a
// full-canvas transform is ~2 Mpx — and this keeps them inside a frame budget.
int rowBandCount(size_t pixels) {
    if (pixels < 150000)
        return 1;
    static const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int cap = int(std::min(hw, 8u));
    return std::max(1, std::min(cap, int(pixels / 75000)));
}

namespace {

// RGBA pass: premultiplied filtering, so a transparent neighbour can only
// fade an edge, never darken it.
template <Interpolation F>
FloatRegion resampleRgba(const FloatRegion &src, const Rect &dstRect, const Mat3 &inv,
                         bool affine) {
    constexpr int T = Kernel<F>::taps;
    FloatRegion out;
    out.bounds = dstRect;
    out.px.assign(size_t(dstRect.w) * size_t(dstRect.h), PixelRGBA8{0, 0, 0, 0});
    if (src.empty())
        return out;

    const Rect sb = src.bounds;
    auto fetch = [&](int x, int y, double w, double *acc) {
        if (x < sb.x || y < sb.y || x >= sb.x + sb.w || y >= sb.y + sb.h)
            return; // outside the lifted region reads transparent
        const PixelRGBA8 p = src.px[size_t(y - sb.y) * size_t(sb.w) + size_t(x - sb.x)];
        const double a = double(p.a) * kInv255;
        if (a <= 0.0)
            return;
        acc[0] += w * (double(p.r) * kInv255) * a;
        acc[1] += w * (double(p.g) * kInv255) * a;
        acc[2] += w * (double(p.b) * kInv255) * a;
        acc[3] += w * a;
    };

    const double *m = inv.m;
    const double x0c = double(dstRect.x) + 0.5;
    // Affine maps step along the row instead of dividing by w per pixel; and
    // when the source row does not drift with x (m[3] == 0) its taps are
    // computed once per scanline. Rotation/shear set m[3], so they fall back to
    // per-pixel taps — hoisting them there would read the wrong rows.
    const bool rowConstant = affine && m[3] == 0.0;
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double yc = double(dstRect.y + y) + 0.5;
            double ax = m[0] * x0c + m[1] * yc + m[2];
            double ay = m[3] * x0c + m[4] * yc + m[5];
            int yf[T];
            double wy[T];
            Kernel<F>::weights(ay - 0.5, yf, wy);
            PixelRGBA8 *row = &out.px[size_t(y) * size_t(dstRect.w)];
            for (int x = 0; x < dstRect.w; ++x) {
                const Vec2 s = affine ? Vec2{ax, ay} : inv.map({double(dstRect.x + x) + 0.5, yc});
                ax += m[0];
                ay += m[3];
                if (!rowConstant)
                    Kernel<F>::weights(s.y - 0.5, yf, wy);
                int xf[T];
                double wx[T];
                Kernel<F>::weights(s.x - 0.5, xf, wx);
                double acc[4] = {0, 0, 0, 0};
                accumulateTaps<T, T>(xf, wx, yf, wy, acc, fetch);
                const double a = std::clamp(acc[3], 0.0, 1.0);
                if (a <= 0.0)
                    continue;
                const double ia = 255.0 / a;
                PixelRGBA8 &o = row[x];
                o.r = quantize255(acc[0] * ia);
                o.g = quantize255(acc[1] * ia);
                o.b = quantize255(acc[2] * ia);
                o.a = quantize255(a * 255.0);
            }
        }
    };
    runRowBands(dstRect.h, out.px.size(), rows);
    return out;
}

// Coverage pass: same chain for an 8-bit mask, so a selection can follow the
// pixels it clips.
template <Interpolation F>
void resampleCoverageCore(const uint8_t *src, int sw, int sh, const Rect &dstRect, const Mat3 &inv,
                          bool affine, uint8_t *dstOut) {
    constexpr int T = Kernel<F>::taps;
    auto fetch = [&](int x, int y, double w, double *acc) {
        if (x < 0 || y < 0 || x >= sw || y >= sh)
            return;
        acc[0] += w * (double(src[size_t(y) * size_t(sw) + size_t(x)]) * kInv255);
    };
    const double *m = inv.m;
    const double x0c = double(dstRect.x) + 0.5;
    const bool rowConstant = affine && m[3] == 0.0;
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double yc = double(dstRect.y + y) + 0.5;
            double ax = m[0] * x0c + m[1] * yc + m[2];
            double ay = m[3] * x0c + m[4] * yc + m[5];
            int yf[T];
            double wy[T];
            Kernel<F>::weights(ay - 0.5, yf, wy);
            uint8_t *row = dstOut + size_t(y) * size_t(dstRect.w);
            for (int x = 0; x < dstRect.w; ++x) {
                const Vec2 s = affine ? Vec2{ax, ay} : inv.map({double(dstRect.x + x) + 0.5, yc});
                ax += m[0];
                ay += m[3];
                if (!rowConstant)
                    Kernel<F>::weights(s.y - 0.5, yf, wy);
                int xf[T];
                double wx[T];
                Kernel<F>::weights(s.x - 0.5, xf, wx);
                double acc[1] = {0.0};
                accumulateTaps<T, T>(xf, wx, yf, wy, acc, fetch);
                row[x] = quantize255(acc[0] * 255.0);
            }
        }
    };
    runRowBands(dstRect.h, size_t(dstRect.w) * size_t(dstRect.h), rows);
}

} // namespace

// --- resampling -------------------------------------------------------------

PixelRGBA8 sampleRegionBilinear(const FloatRegion &src, Vec2 p) {
    PixelRGBA8 out{0, 0, 0, 0};
    if (src.empty() || !std::isfinite(p.x) || !std::isfinite(p.y))
        return out;
    const double u = p.x - 0.5, v = p.y - 0.5;
    const int i0 = floorToInt(u), j0 = floorToInt(v);
    const double fx = u - double(i0), fy = v - double(j0);
    double acc[4] = {0, 0, 0, 0};
    const Rect b = src.bounds;
    for (int j = 0; j < 2; ++j) {
        const double wy = j ? fy : 1.0 - fy;
        const int y = j0 + j;
        if (wy == 0.0 || y < b.y || y >= b.y + b.h)
            continue;
        for (int i = 0; i < 2; ++i) {
            const double w = (i ? fx : 1.0 - fx) * wy;
            const int x = i0 + i;
            if (w == 0.0 || x < b.x || x >= b.x + b.w)
                continue;
            const PixelRGBA8 s = src.px[size_t(y - b.y) * size_t(b.w) + size_t(x - b.x)];
            const double a = double(s.a) * kInv255;
            if (a <= 0.0)
                continue;
            acc[0] += w * (double(s.r) * kInv255) * a;
            acc[1] += w * (double(s.g) * kInv255) * a;
            acc[2] += w * (double(s.b) * kInv255) * a;
            acc[3] += w * a;
        }
    }
    const double a = std::clamp(acc[3], 0.0, 1.0);
    if (a <= 0.0)
        return out;
    const double ia = 255.0 / a;
    out.r = quantize255(acc[0] * ia);
    out.g = quantize255(acc[1] * ia);
    out.b = quantize255(acc[2] * ia);
    out.a = quantize255(a * 255.0);
    return out;
}

FloatRegion resampleRegion(const FloatRegion &src, const Rect &dstRect, const Mat3 &forward,
                           Interpolation filter) {
    if (dstRect.isEmpty())
        return FloatRegion{};
    bool ok = false;
    const Mat3 inv = forward.inverted(&ok);
    if (!ok)
        return FloatRegion{};
    const bool affine = inv.m[6] == 0.0 && inv.m[7] == 0.0 && inv.m[8] == 1.0;
    switch (filter) {
    case Interpolation::Nearest:
        return resampleRgba<Interpolation::Nearest>(src, dstRect, inv, affine);
    case Interpolation::Bilinear:
        return resampleRgba<Interpolation::Bilinear>(src, dstRect, inv, affine);
    case Interpolation::Bicubic:
        return resampleRgba<Interpolation::Bicubic>(src, dstRect, inv, affine);
    default:
        return resampleRgba<Interpolation::Lanczos3>(src, dstRect, inv, affine);
    }
}

void resampleCoverage(const uint8_t *src, int sw, int sh, const Rect &dstRect, const Mat3 &forward,
                      Interpolation filter, uint8_t *dstOut) {
    if (!src || !dstOut || dstRect.isEmpty() || sw <= 0 || sh <= 0)
        return;
    bool ok = false;
    const Mat3 inv = forward.inverted(&ok);
    if (!ok) {
        std::fill(dstOut, dstOut + size_t(dstRect.w) * size_t(dstRect.h), uint8_t(0));
        return;
    }
    const bool affine = inv.m[6] == 0.0 && inv.m[7] == 0.0 && inv.m[8] == 1.0;
    switch (filter) {
    case Interpolation::Nearest:
        resampleCoverageCore<Interpolation::Nearest>(src, sw, sh, dstRect, inv, affine, dstOut);
        break;
    case Interpolation::Bilinear:
        resampleCoverageCore<Interpolation::Bilinear>(src, sw, sh, dstRect, inv, affine, dstOut);
        break;
    case Interpolation::Bicubic:
        resampleCoverageCore<Interpolation::Bicubic>(src, sw, sh, dstRect, inv, affine, dstOut);
        break;
    default:
        resampleCoverageCore<Interpolation::Lanczos3>(src, sw, sh, dstRect, inv, affine, dstOut);
        break;
    }
}

Rect mappedBounds(const FloatRegion &src, const Mat3 &forward) {
    if (src.empty())
        return {};
    bool ok = false;
    forward.inverted(&ok); // singular maps have no meaningful bounds
    if (!ok)
        return {};
    Vec2 c[4], d[4];
    rectCorners(src.bounds, c);
    for (int i = 0; i < 4; ++i) {
        d[i] = forward.map(c[i]);
        if (!std::isfinite(d[i].x) || !std::isfinite(d[i].y))
            return {};
    }
    return cornersBounds(d);
}

FloatRegion rotateRegionQuarter(const FloatRegion &src, int quarterTurns) {
    const int turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0 || src.empty())
        return src;
    const int w = src.bounds.w, h = src.bounds.h;
    const bool odd = (turns % 2) != 0;
    FloatRegion out;
    out.bounds = {src.bounds.x, src.bounds.y, odd ? h : w, odd ? w : h};
    out.px.resize(size_t(out.bounds.w) * size_t(out.bounds.h));
    const int dw = out.bounds.w;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const PixelRGBA8 p = src.px[size_t(j) * size_t(w) + size_t(i)];
            // 90-degree counter-clockwise steps on screen (y grows downward):
            // one step maps (i, j) -> (j, w-1-i) in a block of size h x w.
            int di = 0, dj = 0;
            switch (turns) {
            case 1: di = j; dj = w - 1 - i; break;
            case 2: di = w - 1 - i; dj = h - 1 - j; break;
            default: di = h - 1 - j; dj = i; break;
            }
            out.px[size_t(dj) * size_t(dw) + size_t(di)] = p;
        }
    }
    return out;
}

} // namespace ink
