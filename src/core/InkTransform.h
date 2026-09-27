#pragma once
// Affine + projective resampling: the pixel engine behind the transform tools.
// Clean-room take on Krita's KisTransformWorker/KisPerspectiveTransformWorker
// concept: ONE inverse-mapping sampler with pluggable 1-D kernels, exact at
// integer offsets (so translation and 90-degree steps are lossless), filtered
// premultiplied so transparent neighbours can never darken an edge.
// Pure C++20 — no Qt — so the transform math stays headless unit-testable.
#include "core/InkRegion.h"        // FloatRegion, Rect
#include "core/InkViewConverter.h" // Vec2, Affine2D
#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace ink {

enum class Interpolation { Nearest, Bilinear, Bicubic, Lanczos3 };

// Row-major 3x3 projective map of (x, y, 1) with an implicit unit corner.
struct Mat3 {
    double m[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};

    static Mat3 identity() { return {}; }
    static Mat3 fromAffine(const Affine2D &a);
    static Mat3 translation(double dx, double dy);
    static Mat3 scale(double sx, double sy);
    static Mat3 rotation(double degrees);
    static Mat3 shear(double kx, double ky);

    Vec2 map(Vec2 p) const;
    Mat3 multiply(const Mat3 &o) const; // this ∘ o (o is applied first)
    Mat3 inverted(bool *ok = nullptr) const;
    bool isIdentity(double eps = 1e-9) const;
};

// The four corners of a rect in the canonical order used everywhere here:
// 0 = top-left, 1 = top-right, 2 = bottom-right, 3 = bottom-left.
void rectCorners(const Rect &r, Vec2 out[4]);
Rect cornersBounds(const Vec2 q[4]);

// Homography taking the four `from` corners onto the four `to` corners
// (unit-square solve per quad, then H = B·A⁻¹ — Krita's getTransitionMatrix).
bool quadToQuad(const Vec2 from[4], const Vec2 to[4], Mat3 &out);

// The free transform's parameter block: scalars rather than a matrix, so the
// W/H/angle/shear readouts stay honest, plus an optional projective override
// installed by a "free corner" drag (Krita's flattened perspective term).
// Matrix order: subtract the pivot, scale, shear, rotate, add it back, then
// translate; the projective override acts first, in source space.
struct FreeTransformParams {
    Vec2 pivot;
    Vec2 translate;
    double scaleX{1.0}, scaleY{1.0};
    double angleDeg{0.0};
    double shearX{0.0}, shearY{0.0};
    bool hasQuad{false};
    Vec2 quadSrc[4]{};

    void reset();
    // `sourceRect` is the lifted region: its corners anchor the override.
    Mat3 matrix(const Rect &sourceRect) const;
};

// Resample `src` through `forward` into a region covering `dstRect` (document
// coordinates). Samples outside `src` read transparent black.
FloatRegion resampleRegion(const FloatRegion &src, const Rect &dstRect, const Mat3 &forward,
                           Interpolation filter);

// Integer rect enclosing the mapped source corners; empty when the map is
// singular or the result is not finite.
Rect mappedBounds(const FloatRegion &src, const Mat3 &forward);

// True when the map takes pixel centres to pixel centres: translations, flips,
// integer scales and 90-degree turns about a half-integer pivot. Nearest-
// neighbour is then not an approximation but the exact, lossless answer — which
// is why dragging a layer by whole pixels never softens it. Shared by the
// transform tool and the frame compositor so both make the same call.
inline bool isLatticeMap(const Mat3 &m) {
    for (int i = 0; i < 9; ++i)
        if (std::abs(m.m[i] - std::round(m.m[i])) > 1e-9) return false;
    return true;
}

// Lossless quarter turns (0..3, counter-clockwise): no filtering at all.
FloatRegion rotateRegionQuarter(const FloatRegion &src, int quarterTurns);

// The same filter chain for an 8-bit coverage mask, so a selection can follow
// the pixels it clips. `dstOut` must hold dstRect.w * dstRect.h bytes.
void resampleCoverage(const uint8_t *src, int sw, int sh, const Rect &dstRect,
                      const Mat3 &forward, Interpolation filter, uint8_t *dstOut);

// Bilinear sample at a document-space point, premultiplied (so a transparent
// neighbour can only fade an edge, never darken it). Transparent outside `src`.
// Free-form deformations (warp, liquify) have no matrix to invert, so they
// back-map per pixel and sample through this.
PixelRGBA8 sampleRegionBilinear(const FloatRegion &src, Vec2 p);

// Row-band parallelism shared by the resamplers: `body(y0, y1)` runs on
// disjoint row ranges of an immutable source, so the result is bit-identical
// to a single-threaded pass while a full-canvas preview stays inside a frame.
int rowBandCount(size_t pixels);

template <typename Body>
void runRowBands(int rows, size_t pixels, Body &&body) {
    const int n = std::min(rowBandCount(pixels), std::max(1, rows));
    if (n <= 1) {
        body(0, rows);
        return;
    }
    const int band = (rows + n - 1) / n;
    std::vector<std::thread> pool;
    pool.reserve(size_t(n - 1));
    for (int b = 1; b < n; ++b) {
        const int y0 = b * band;
        const int y1 = std::min(rows, y0 + band);
        if (y0 >= y1)
            break;
        pool.emplace_back([&body, y0, y1] { body(y0, y1); });
    }
    body(0, std::min(rows, band));
    for (std::thread &t : pool)
        t.join();
}

} // namespace ink
