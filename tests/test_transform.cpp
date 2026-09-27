// Transform core: the resampler must be exact wherever it can be (identity,
// integer translation, 90-degree steps, integer scale steps) and must filter
// premultiplied, so a transparent neighbour can never darken an edge.
#include "core/InkTransform.h"
#include "test_check.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ink;
using Clock = std::chrono::steady_clock;

namespace {

FloatRegion makeRegion(const Rect &b, uint32_t seed = 1) {
    FloatRegion r;
    r.bounds = b;
    r.px.resize(size_t(b.w) * size_t(b.h));
    uint32_t s = seed;
    for (size_t i = 0; i < r.px.size(); ++i) {
        s = s * 1664525u + 1013904223u;
        r.px[i] = {uint8_t(s >> 24), uint8_t(s >> 16), uint8_t(s >> 8), uint8_t(200 + (s >> 30))};
    }
    return r;
}

bool samePixels(const FloatRegion &a, const FloatRegion &b, const char *what = "") {
    if (a.bounds.w != b.bounds.w || a.bounds.h != b.bounds.h || a.px.size() != b.px.size()) {
        std::printf("  [%s] size %dx%d vs %dx%d\n", what, a.bounds.w, a.bounds.h, b.bounds.w,
                    b.bounds.h);
        return false;
    }
    for (size_t i = 0; i < a.px.size(); ++i) {
        if (a.px[i].r != b.px[i].r || a.px[i].g != b.px[i].g || a.px[i].b != b.px[i].b ||
            a.px[i].a != b.px[i].a) {
            std::printf("  [%s] first diff at %zu: got %u,%u,%u,%u want %u,%u,%u,%u\n", what, i,
                        a.px[i].r, a.px[i].g, a.px[i].b, a.px[i].a, b.px[i].r, b.px[i].g, b.px[i].b,
                        b.px[i].a);
            return false;
        }
    }
    return true;
}

const char *filterName(Interpolation f) {
    switch (f) {
    case Interpolation::Nearest: return "nearest";
    case Interpolation::Bilinear: return "bilinear";
    case Interpolation::Bicubic: return "bicubic";
    default: return "lanczos3";
    }
}

const Interpolation kFilters[] = {Interpolation::Nearest, Interpolation::Bilinear,
                                  Interpolation::Bicubic, Interpolation::Lanczos3};

} // namespace

int main() {
    const Rect b{0, 0, 37, 23};
    const FloatRegion src = makeRegion(b, 7);

    // 1. Identity is bit-exact for every kernel (all kernels interpolate at
    //    integer offsets, so a pixel centre lands exactly on a source pixel).
    for (Interpolation f : kFilters) {
        const FloatRegion out = resampleRegion(src, b, Mat3::identity(), f);
        INK_CHECK(out.bounds.w == b.w && out.bounds.h == b.h);
        INK_CHECK(samePixels(out, src, filterName(f)));
    }
    std::printf("identity_exact OK\n");

    // 2. Integer translation is a lossless copy, and pixels that leave the
    //    source read transparent.
    for (Interpolation f : kFilters) {
        const Rect dst{6, -4, b.w, b.h};
        const FloatRegion out = resampleRegion(src, dst, Mat3::translation(6, -4), f);
        for (int y = 0; y < b.h; ++y) {
            for (int x = 0; x < b.w; ++x) {
                const PixelRGBA8 got = out.px[size_t(y) * size_t(b.w) + size_t(x)];
                const PixelRGBA8 want = src.px[size_t(y) * size_t(b.w) + size_t(x)];
                INK_CHECK(got.r == want.r && got.g == want.g && got.b == want.b && got.a == want.a);
            }
        }
    }
    // Samples that fall outside the lifted region read transparent.
    const FloatRegion off = resampleRegion(src, {500, 500, 4, 4}, Mat3::identity(),
                                           Interpolation::Bicubic);
    for (const PixelRGBA8 &p : off.px)
        INK_CHECK(p.a == 0);
    std::printf("translate_lossless OK\n");

    // 3. Nearest 2x scale is an exact blocky copy.
    {
        const Rect dst{0, 0, b.w * 2, b.h * 2};
        const FloatRegion out = resampleRegion(src, dst, Mat3::scale(2, 2), Interpolation::Nearest);
        for (int y = 0; y < dst.h; ++y) {
            for (int x = 0; x < dst.w; ++x) {
                const PixelRGBA8 got = out.px[size_t(y) * size_t(dst.w) + size_t(x)];
                const PixelRGBA8 want = src.px[size_t(y / 2) * size_t(b.w) + size_t(x / 2)];
                INK_CHECK(got.r == want.r && got.a == want.a);
            }
        }
    }
    std::printf("nearest_scale_exact OK\n");

    // 4. Quarter turns: exact permutation, block anchored at the source origin,
    //    and four turns come back to the original pixels.
    {
        const Rect sb{0, 0, 3, 2};
        const FloatRegion s = makeRegion(sb, 11);
        const FloatRegion q1 = rotateRegionQuarter(s, 1);
        INK_CHECK(q1.bounds.w == 2 && q1.bounds.h == 3);
        for (int j = 0; j < sb.h; ++j) {
            for (int i = 0; i < sb.w; ++i) {
                const PixelRGBA8 want = s.px[size_t(j) * size_t(sb.w) + size_t(i)];
                const PixelRGBA8 got = q1.px[size_t(2 - i) * size_t(2) + size_t(j)];
                INK_CHECK(got.r == want.r && got.g == want.g && got.b == want.b && got.a == want.a);
            }
        }
        INK_CHECK(samePixels(rotateRegionQuarter(s, 4), s));
        INK_CHECK(samePixels(rotateRegionQuarter(rotateRegionQuarter(s, 3), 1), s));
        INK_CHECK(samePixels(rotateRegionQuarter(rotateRegionQuarter(s, 1), 3), s));

        // 5. The generic resampler agrees with the lossless path for a
        //    counter-clockwise step about the region origin: (x, y) -> (y, 3-x).
        Mat3 m;
        m.m[0] = 0; m.m[1] = 1; m.m[2] = 0;
        m.m[3] = -1; m.m[4] = 0; m.m[5] = 3;
        const FloatRegion viaMatrix =
            resampleRegion(s, q1.bounds, m, Interpolation::Nearest);
        INK_CHECK(samePixels(viaMatrix, q1));
    }
    std::printf("quarter_turn_exact OK\n");

    // 6. quadToQuad: a translated quad is a translation, and every corner maps
    //    exactly onto its destination (trapezoid included).
    {
        Vec2 from[4], to[4];
        rectCorners({0, 0, 10, 10}, from);
        for (int i = 0; i < 4; ++i)
            to[i] = {from[i].x + 5, from[i].y + 7};
        Mat3 m;
        INK_CHECK(quadToQuad(from, to, m));
        INK_CHECK(m.isIdentity(1e-9) == false);
        const Vec2 p = m.map({3.5, 4.5});
        INK_CHECK(std::abs(p.x - 8.5) < 1e-9 && std::abs(p.y - 11.5) < 1e-9);

        const Vec2 trap[4] = {{0, 0}, {10, 0}, {8, 10}, {2, 10}};
        INK_CHECK(quadToQuad(from, trap, m));
        for (int i = 0; i < 4; ++i) {
            const Vec2 q = m.map(from[i]);
            INK_CHECK(std::abs(q.x - trap[i].x) < 1e-9);
            INK_CHECK(std::abs(q.y - trap[i].y) < 1e-9);
        }
        // Round trip: forward then inverse lands back on the source corners.
        Mat3 back;
        INK_CHECK(quadToQuad(trap, from, back));
        for (int i = 0; i < 4; ++i) {
            const Vec2 q = back.map(m.map(from[i]));
            INK_CHECK(std::abs(q.x - from[i].x) < 1e-8 && std::abs(q.y - from[i].y) < 1e-8);
        }
        // Degenerate (collapsed) quads are refused instead of dividing by zero.
        const Vec2 collapsed[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        INK_CHECK(!quadToQuad(from, collapsed, m));
    }
    std::printf("quad_to_quad OK\n");

    // 7. Premultiplied filtering: half-way between opaque red and transparent
    //    must stay red (straight-alpha filtering would drag it toward black).
    {
        FloatRegion e;
        e.bounds = {0, 0, 2, 1};
        e.px = {{255, 0, 0, 255}, {0, 0, 0, 0}};
        const FloatRegion out = resampleRegion(e, {1, 0, 1, 1}, Mat3::translation(0.5, 0),
                                              Interpolation::Bilinear);
        const PixelRGBA8 p = out.px[0];
        INK_CHECK(p.r >= 250);
        INK_CHECK(p.a >= 120 && p.a <= 136);
        std::printf("premultiplied_edge r=%d a=%d OK\n", int(p.r), int(p.a));
    }

    // 7b. Free transform parameters: the matrix the tool builds from its
    //     fields, including the Ctrl+corner "free corner" override. The tool
    //     installs the override by inverse-mapping the pointer into source
    //     space, so that is exactly what is checked here.
    {
        const Rect box{10, 20, 40, 30};
        Vec2 from[4];
        rectCorners(box, from);
        const Vec2 centre{double(box.x) + box.w * 0.5, double(box.y) + box.h * 0.5};

        FreeTransformParams p;
        p.pivot = centre;
        INK_CHECK(p.matrix(box).isIdentity(1e-9));

        p.translate = {7.0, -3.0};
        Vec2 m0 = p.matrix(box).map(from[0]);
        INK_CHECK(std::abs(m0.x - (from[0].x + 7.0)) < 1e-9);
        INK_CHECK(std::abs(m0.y - (from[0].y - 3.0)) < 1e-9);

        // Rotate 90 degrees about the pivot, scale, then shear: check the
        // pivot itself is the fixed point and a corner lands where it should.
        p.reset();
        p.pivot = centre;
        p.angleDeg = 90.0;
        Mat3 r = p.matrix(box);
        const Vec2 fixed = r.map(centre);
        INK_CHECK(std::abs(fixed.x - centre.x) < 1e-9 && std::abs(fixed.y - centre.y) < 1e-9);
        Vec2 c0 = r.map({centre.x + 10.0, centre.y});
        INK_CHECK(std::abs(c0.x - centre.x) < 1e-9 && std::abs(c0.y - (centre.y + 10.0)) < 1e-9);

        p.reset();
        p.pivot = centre;
        p.scaleX = 2.0;
        p.scaleY = 0.5;
        Vec2 s0 = p.matrix(box).map({centre.x + 10.0, centre.y + 10.0});
        INK_CHECK(std::abs(s0.x - (centre.x + 20.0)) < 1e-9);
        INK_CHECK(std::abs(s0.y - (centre.y + 5.0)) < 1e-9);

        // Ctrl+corner: drag corner 2 to `target` while the frame carries an
        // arbitrary affine state. The dragged corner must land on the pointer
        // and the opposite corner must stay anchored.
        p.reset();
        p.pivot = centre;
        p.scaleX = 1.7;
        p.scaleY = 0.6;
        p.angleDeg = 25.0;
        p.shearX = 0.2;
        const Mat3 affine = p.matrix(box);
        const Vec2 target{140.0, 95.0};
        p.hasQuad = true;
        rectCorners(box, p.quadSrc);
        p.quadSrc[2] = affine.inverted().map(target);
        const Mat3 m = p.matrix(box);
        const Vec2 got = m.map(from[2]);
        INK_CHECK(std::abs(got.x - target.x) < 1e-8 && std::abs(got.y - target.y) < 1e-8);
        const Vec2 anchor = m.map(from[0]);
        const Vec2 anchorAffine = affine.map(from[0]);
        INK_CHECK(std::abs(anchor.x - anchorAffine.x) < 1e-8);
        INK_CHECK(std::abs(anchor.y - anchorAffine.y) < 1e-8);
        std::printf("free_params OK\n");
    }

    // 8. mappedBounds encloses every mapped corner and refuses singular maps.
    {
        const Rect dst = mappedBounds(src, Mat3::rotation(30));
        Vec2 c[4], d[4];
        rectCorners(b, c);
        for (int i = 0; i < 4; ++i) {
            d[i] = Mat3::rotation(30).map(c[i]);
            INK_CHECK(d[i].x >= double(dst.x) && d[i].x <= double(dst.x + dst.w));
            INK_CHECK(d[i].y >= double(dst.y) && d[i].y <= double(dst.y + dst.h));
        }
        INK_CHECK(mappedBounds(src, Mat3::scale(0, 0)).isEmpty());
    }
    std::printf("mapped_bounds OK\n");

    // 9. Coverage masks follow the same chain (used to carry a selection).
    {
        const int W = 16, H = 12;
        std::vector<uint8_t> mask(size_t(W) * H, 0);
        for (int y = 2; y < 7; ++y)
            for (int x = 3; x < 9; ++x)
                mask[size_t(y) * W + size_t(x)] = 255;
        std::vector<uint8_t> out(size_t(W) * H, 0);
        resampleCoverage(mask.data(), W, H, {0, 0, W, H}, Mat3::translation(2, 1),
                         Interpolation::Nearest, out.data());
        // The mask must land 2 right and 1 down; what leaves the canvas is 0.
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint8_t want =
                    (x >= 2 && y >= 1) ? mask[size_t(y - 1) * W + size_t(x - 2)] : uint8_t(0);
                INK_CHECK(out[size_t(y) * W + size_t(x)] == want);
            }
        }

        // Scaling the mask up 2x with bilinear keeps the interior solid and the
        // outside empty.
        std::vector<uint8_t> up(size_t(W * 2) * size_t(H * 2), 128);
        resampleCoverage(mask.data(), W, H, {0, 0, W * 2, H * 2}, Mat3::scale(2, 2),
                         Interpolation::Bilinear, up.data());
        INK_CHECK(up[size_t(8) * size_t(W * 2) + size_t(10)] == 255); // inside (5,4)
        INK_CHECK(up[0] == 0);
    }
    std::printf("coverage_follows OK\n");

    // 10. Perf sanity for the interactive drag path: 1 Mpx source, 1.5 Mpx
    //     destination, one timing per kernel.
    {
        const FloatRegion big = makeRegion({0, 0, 1024, 1024}, 3);
        double ms[4] = {0, 0, 0, 0};
        // Best of two passes per kernel: scheduler noise must not turn a
        // regression guard into a coin flip.
        for (int pass = 0; pass < 2; ++pass) {
            for (int f = 0; f < 4; ++f) {
                auto t0 = Clock::now();
                const FloatRegion out = resampleRegion(big, {0, 0, 1228, 1228}, Mat3::scale(1.2, 1.2),
                                                       kFilters[f]);
                const double one =
                    std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
                if (pass == 0 || one < ms[f]) ms[f] = one;
                INK_CHECK(!out.empty());
            }
        }
        std::printf("perf 1.5Mpx nearest=%.1fms bilinear=%.1fms bicubic=%.1fms lanczos=%.1fms\n",
                    ms[0], ms[1], ms[2], ms[3]);
// Regression guard for the interactive drag path. The budget follows the build:
// debug builds run this unoptimized and are several times slower.
#ifdef NDEBUG
        const double budget = 150.0;
#else
        const double budget = 600.0;
#endif
        INK_CHECK(ms[1] < budget);
    }

    std::printf("test_transform PASS\n");
    return 0;
}
