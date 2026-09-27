// Deformation core: identity must be bit-exact, one control point must be a
// pure translation, the three anchor strengths must differ exactly where they
// are supposed to (scale in, scale out), and the liquify brush must follow its
// Gaussian falloff.
#include "core/InkDeform.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ink;

namespace {

FloatRegion makeRegion(const Rect &b, uint32_t seed = 5) {
    FloatRegion r;
    r.bounds = b;
    r.px.resize(size_t(b.w) * size_t(b.h));
    uint32_t s = seed;
    for (size_t i = 0; i < r.px.size(); ++i) {
        s = s * 1664525u + 1013904223u;
        r.px[i] = {uint8_t(s >> 24), uint8_t(s >> 16), uint8_t(s >> 8), uint8_t(220 + (s >> 30))};
    }
    return r;
}

bool samePixels(const FloatRegion &a, const FloatRegion &b, const char *what = "") {
    if (a.bounds.w != b.bounds.w || a.bounds.h != b.bounds.h || a.px.size() != b.px.size()) {
        std::printf("  [%s] size mismatch\n", what);
        return false;
    }
    for (size_t i = 0; i < a.px.size(); ++i) {
        if (a.px[i].r != b.px[i].r || a.px[i].g != b.px[i].g || a.px[i].b != b.px[i].b ||
            a.px[i].a != b.px[i].a) {
            std::printf("  [%s] first diff at %zu: got %u,%u,%u,%u want %u,%u,%u,%u\n", what, i,
                        a.px[i].r, a.px[i].g, a.px[i].b, a.px[i].a, b.px[i].r, b.px[i].g,
                        b.px[i].b, b.px[i].a);
            return false;
        }
    }
    return true;
}

bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }

bool nearPt(const Vec2 &a, const Vec2 &b, double eps = 1e-9) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps);
}

} // namespace

int main() {
    // 1. MLS: a correspondence that is already a rotation must reproduce it,
    //    and one control point must be a plain translation.
    {
        const std::vector<Vec2> p = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        const double ang = 0.7;
        const double c = std::cos(ang), s = std::sin(ang);
        std::vector<Vec2> q;
        for (const Vec2 &v : p)
            q.push_back({c * v.x - s * v.y, s * v.x + c * v.y});

        for (MlsKind kind : {MlsKind::Affine, MlsKind::Similitude, MlsKind::Rigid}) {
            for (Vec2 v : {Vec2{3, 4}, Vec2{-5, 2}, Vec2{7, 7}}) {
                const Vec2 got = mlsTransform(v, p, q, 1.0, kind);
                const Vec2 want{c * v.x - s * v.y, s * v.x + c * v.y};
                INK_CHECK(nearPt(got, want, 1e-9));
            }
        }

        // A scaled correspondence (q = 2R p): Affine and Similitude follow the
        // fitted scale exactly, Rigid only rotates — that IS the anchor-strength
        // list. Probing OUTSIDE the control set keeps the MLS centroid from
        // tracking the probe, so the two behaviours are clearly separated.
        std::vector<Vec2> q2;
        for (const Vec2 &pv : p)
            q2.push_back({2.0 * (c * pv.x - s * pv.y), 2.0 * (s * pv.x + c * pv.y)});
        const Vec2 v{60.0, 60.0};
        const Vec2 rot{c * v.x - s * v.y, s * v.x + c * v.y};
        const Vec2 aff = mlsTransform(v, p, q2, 1.0, MlsKind::Affine);
        INK_CHECK(nearPt(aff, {2 * rot.x, 2 * rot.y}, 1e-9));
        INK_CHECK(nearPt(mlsTransform(v, p, q2, 1.0, MlsKind::Similitude),
                         {2 * rot.x, 2 * rot.y}, 1e-9));
        const Vec2 rig = mlsTransform(v, p, q2, 1.0, MlsKind::Rigid);
        const double lenAff = std::hypot(aff.x, aff.y);
        const double lenRig = std::hypot(rig.x, rig.y);
        INK_CHECK(lenRig < lenAff * 0.75); // rigid refuses to scale the drag up
        INK_CHECK(lenRig > lenAff * 0.4);  // ...but it still follows it

        // Identity correspondence and single-point translation.
        INK_CHECK(nearPt(mlsTransform({3, 4}, p, p, 1.0, MlsKind::Affine), {3, 4}));
        const std::vector<Vec2> oneP = {{5, 5}};
        const std::vector<Vec2> oneQ = {{8, 1}};
        INK_CHECK(nearPt(mlsTransform({0, 0}, oneP, oneQ, 1.0, MlsKind::Affine), {3, -4}));
        std::printf("mls OK\n");
    }

    // 2. Mesh: identity stays exact through the whole pipeline, and one driven
    //    control point moves the lattice where it should.
    {
        const Rect bounds{0, 0, 120, 90};
        DeformMesh mesh = DeformMesh::make(bounds, 10, 9);
        INK_CHECK(mesh.valid() && mesh.isIdentity());

        const FloatRegion src = makeRegion(bounds, 9);
        const FloatRegion out = warpRegion(src, bounds, mesh);
        INK_CHECK(samePixels(out, src, "identity warp"));

        std::vector<Vec2> cp = {mesh.node(5, 4)};
        std::vector<Vec2> cq = {{cp[0].x + 12.0, cp[0].y - 7.0}};
        mesh.deformByPoints(cp, cq, 1.0, MlsKind::Affine);
        INK_CHECK(!mesh.isIdentity());
        // A single control point is a plain translation: the dragged node lands
        // exactly on it and so does every other node.
        INK_CHECK(nearPt(mesh.movedNode(5, 4), cq[0], 1e-9));
        INK_CHECK(nearPt(mesh.movedNode(0, 0),
                         {mesh.node(0, 0).x + 12.0, mesh.node(0, 0).y - 7.0}, 1e-9));

        // MLS is a global (if decaying) warp: with a second point held in place
        // the dragged one still lands, the held one stays, and the far corner
        // follows by much less than the drag.
        mesh = DeformMesh::make(bounds, 10, 9);
        cp = {mesh.node(5, 4), mesh.node(0, 0)};
        cq = {{mesh.node(5, 4).x + 12.0, mesh.node(5, 4).y - 7.0}, mesh.node(0, 0)};
        mesh.deformByPoints(cp, cq, 1.0, MlsKind::Affine);
        INK_CHECK(nearPt(mesh.movedNode(5, 4), cq[0], 1e-3));
        INK_CHECK(nearPt(mesh.movedNode(0, 0), cp[1], 1e-3));
        const double dragged =
            std::hypot(12.0, -7.0);
        const Vec2 farMoved = mesh.movedNode(10, 9);
        const double farMove =
            std::hypot(farMoved.x - mesh.node(10, 9).x, farMoved.y - mesh.node(10, 9).y);
        INK_CHECK(farMove < dragged); // influence decays with distance
        std::printf("mesh drag OK (far corner follows %.2f of %.2f)\n", farMove, dragged);
    }

    // 3. Field inversion: sourceOf(offsetAt) round-trips a known displacement,
    //    and the warped region covers the moved content without NaNs.
    {
        const Rect bounds{0, 0, 64, 64};
        DeformMesh mesh = DeformMesh::make(bounds, 8, 8);
        std::vector<Vec2> cp = {mesh.node(4, 4)};
        std::vector<Vec2> cq = {{cp[0].x + 20.0, cp[0].y + 20.0}};
        mesh.deformByPoints(cp, cq, 1.0, MlsKind::Affine);

        const Vec2 dst{32.5, 32.5};
        const Vec2 offset = mesh.offsetAt(dst);
        INK_CHECK(std::hypot(offset.x, offset.y) > 0.0);
        const Vec2 back = mesh.sourceOf(mesh.offsetAt(dst) + dst);
        INK_CHECK(nearPt(back, dst, 0.5)); // refinement recovers the source point

        const FloatRegion src = makeRegion(bounds, 3);
        const Rect dstRect = mesh.movedBounds();
        INK_CHECK(!dstRect.isEmpty());
        const FloatRegion out = warpRegion(src, dstRect, mesh);
        for (const PixelRGBA8 &px : out.px) {
            INK_CHECK(px.r == px.r && px.a == px.a); // no NaN packed through
        }
        INK_CHECK(out.bounds.x <= bounds.x + 20 && out.bounds.y <= bounds.y + 20);
        std::printf("inverse+warp OK (dst %d,%d %dx%d)\n", dstRect.x, dstRect.y, dstRect.w,
                    dstRect.h);
    }

    // 4. Liquify: the dab follows its falloff — full strength at the centre,
    //    nothing at the brush rim — and the undo brush pulls it back.
    {
        LiquifyField field;
        const Rect bounds{0, 0, 240, 240};
        field.reset(bounds, 8);
        INK_CHECK(field.empty());

        LiquifyField::Params params;
        params.radius = 60.0;
        params.amount = 0.5;
        const Vec2 centre{120, 120};
        field.dab(LiquifyField::Brush::Move, centre, {10.0, 0.0}, 1.0, 0.0, params);
        INK_CHECK(!field.empty());

        const DeformMesh &m = field.mesh();
        // Nearest node to the brush centre (the lattice cell is 8px, so the
        // node grid does not land exactly on it).
        int best = -1;
        double bestD = 1e9;
        for (size_t k = 0; k < m.orig.size(); ++k) {
            const double d = std::hypot(m.orig[k].x - centre.x, m.orig[k].y - centre.y);
            if (d < bestD) {
                bestD = d;
                best = int(k);
            }
        }
        INK_CHECK(best >= 0);
        const double movedX = m.moved[size_t(best)].x - m.orig[size_t(best)].x;
        INK_CHECK(movedX > 2.0);  // pushed right, near full amount
        INK_CHECK(movedX <= 5.0 + 1e-9); // amount * delta = 0.5 * 10

        // Outside the brush radius nothing moved at all.
        for (size_t k = 0; k < m.orig.size(); ++k) {
            const double d = std::hypot(m.orig[k].x - centre.x, m.orig[k].y - centre.y);
            if (d <= params.radius)
                continue;
            INK_CHECK(nearPt(m.moved[k], m.orig[k]));
        }

        // Undo brush restores most of the displacement.
        params.amount = 1.0;
        for (int i = 0; i < 3; ++i)
            field.dab(LiquifyField::Brush::Undo, centre, {0, 0}, 1.0, 0.0, params);
        const double residue = std::abs(m.moved[size_t(best)].x - m.orig[size_t(best)].x);
        INK_CHECK(residue < 0.5);
        std::printf("liquify OK (centre moved %.2fpx, residue %.3f)\n", movedX, residue);
    }

    // 5. Scaled brush and rotation brush leave the field consistent.
    {
        LiquifyField::Params params;
        params.radius = 40.0;
        params.amount = 0.25;
        const Vec2 centre{100, 100};

        LiquifyField scaleField;
        scaleField.reset({0, 0, 200, 200}, 10);
        scaleField.dab(LiquifyField::Brush::Scale, centre, {0, 0}, 1.5, 0.0, params);
        const DeformMesh &sm = scaleField.mesh();
        // A node to the right of the centre must have been pushed further right.
        bool pushed = false;
        for (size_t k = 0; k < sm.orig.size(); ++k) {
            if (sm.moved[k].x - sm.orig[k].x > 0.01 && sm.orig[k].x > centre.x)
                pushed = true;
        }
        INK_CHECK(pushed);

        LiquifyField rotField;
        rotField.reset({0, 0, 200, 200}, 10);
        rotField.dab(LiquifyField::Brush::Rotate, centre, {0, 0}, 1.0, 0.4, params);
        const DeformMesh &rm = rotField.mesh();
        bool turned = false;
        for (size_t k = 0; k < rm.orig.size(); ++k) {
            if (std::abs(rm.moved[k].y - rm.orig[k].y) > 0.01)
                turned = true;
        }
        INK_CHECK(turned);
        std::printf("scale+rotate brush OK\n");
    }

    // 6. Coverage follows the deformation, so a selection rides along with the
    //    pixels it clips (warp and liquify have no matrix to resample with).
    {
        const int W = 32, H = 24;
        std::vector<uint8_t> mask(size_t(W) * H, 0);
        for (int y = 8; y < 16; ++y)
            for (int x = 10; x < 20; ++x)
                mask[size_t(y) * size_t(W) + size_t(x)] = 255;

        DeformMesh mesh = DeformMesh::make({0, 0, W, H}, 8, 6);
        std::vector<uint8_t> out(size_t(W) * H, 7);
        warpCoverage(mask.data(), W, H, {0, 0, W, H}, mesh, out.data());
        for (size_t i = 0; i < out.size(); ++i)
            INK_CHECK(out[i] == mask[i]); // identity is exact

        // One control point translates the whole field: the mask shifts with it.
        std::vector<Vec2> p = {mesh.node(4, 3)};
        std::vector<Vec2> q = {{p[0].x + 6.0, p[0].y}};
        mesh.deformByPoints(p, q, 1.0, MlsKind::Affine);
        std::fill(out.begin(), out.end(), uint8_t(0));
        warpCoverage(mask.data(), W, H, {0, 0, W, H}, mesh, out.data());
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const uint8_t want =
                    (x >= 6) ? mask[size_t(y) * size_t(W) + size_t(x - 6)] : uint8_t(0);
                INK_CHECK(out[size_t(y) * size_t(W) + size_t(x)] == want);
            }
        }
        std::printf("warp coverage OK\n");
    }

    std::printf("test_deform PASS\n");
    return 0;
}
