#pragma once
// Free-form deformation for the warp and liquify tools: one control lattice,
// two ways to drive it (MLS point handles, liquify brush strokes), and one
// resampler. Clean-room take on Krita's grid-warping concept (GridIterationTools
// + KisWarpTransformWorker/KisLiquifyTransformWorker): build a displacement
// field on a regular lattice, then invert it per destination pixel by
// fixed-point iteration. Iterating on the field stays correct where a per-cell
// quad inverse breaks down (folds, self-overlap), and it needs no matrix.
#include "core/InkTransform.h"
#include <cstdint>
#include <vector>

namespace ink {

// How local the deformation is allowed to be, i.e. which transform is fitted
// around every control point (Krita's anchor-strength list).
enum class MlsKind { Affine, Similitude, Rigid };

// Moving Least Squares deformation (Schaefer/McPhail/Warren): maps `v` through
// the transform that best fits the weighted correspondences p[i] -> q[i].
// `alpha` is the flexibility — 1 is the paper's default, larger sticks closer
// to each control point.
Vec2 mlsTransform(Vec2 v, const std::vector<Vec2> &p, const std::vector<Vec2> &q, double alpha,
                  MlsKind kind);

// Regular control lattice over `bounds`. `orig` holds the source positions of
// the (cols+1) x (rows+1) nodes, `moved` where each node ends up; both are
// row-major with stride cols+1. Nodes outside `bounds` never move, which is
// what keeps a warp local to the lifted region.
struct DeformMesh {
    Rect bounds;
    int cols{0}, rows{0};
    std::vector<Vec2> orig, moved;

    static DeformMesh make(const Rect &bounds, int cols, int rows);
    bool valid() const {
        return cols > 0 && rows > 0 && orig.size() == moved.size() &&
               orig.size() == size_t(cols + 1) * size_t(rows + 1);
    }
    Vec2 node(int cx, int cy) const; // source position of a node
    Vec2 movedNode(int cx, int cy) const;

    // Drive every node through a control-point correspondence (the warp tool).
    void deformByPoints(const std::vector<Vec2> &p, const std::vector<Vec2> &q, double alpha,
                        MlsKind kind);

    // Displacement of the field at a DESTINATION point (bilinear over the cell).
    Vec2 offsetAt(Vec2 dst) const;
    // Source point that landed on `dst`; the field is inverted by two
    // fixed-point refinement steps, which converges for any sane deformation.
    Vec2 sourceOf(Vec2 dst) const;

    Rect movedBounds() const; // where the deformed content can land
    bool isIdentity(double eps = 1e-6) const;
};

// Resample `src` through the mesh into `dstRect` (document space).
FloatRegion warpRegion(const FloatRegion &src, const Rect &dstRect, const DeformMesh &mesh);

// Same mapping for an 8-bit coverage mask, so a selection can follow the pixels
// it clips through a deformation that has no matrix. `dstOut` holds
// dstRect.w * dstRect.h bytes, `src` is sw * sh with origin (0, 0).
void warpCoverage(const uint8_t *src, int sw, int sh, const Rect &dstRect, const DeformMesh &mesh,
                  uint8_t *dstOut);

// Liquify: the mesh IS the accumulated state; the brush edits `moved` under a
// Gaussian falloff, lambda = exp(-0.5 (d/sigma)^2) with sigma = radius/3, so a
// dab reaches exactly `radius` (Krita's math).
class LiquifyField {
public:
    enum class Brush { Move, Scale, Rotate, Undo };

    struct Params {
        double radius{60.0};
        double amount{0.25};
        double flow{0.5};
        bool wash{false}; // "Wash": blend toward the dab instead of building up
    };

    void reset(const Rect &bounds, int cellPx = 12);
    bool empty() const { return !mesh_.valid() || mesh_.isIdentity(); }
    const DeformMesh &mesh() const { return mesh_; }

    // One dab. Move uses `delta` (pointer movement), Scale `factor`, Rotate
    // `angleRad`; Undo pulls the field back toward the original lattice.
    void dab(Brush brush, Vec2 center, Vec2 delta, double factor, double angleRad,
             const Params &params);

private:
    // Visits every node within `radius` of `center`, giving it its falloff
    // weight; the callback receives the node index and the weight.
    template <typename Fn>
    void forEachNodeInRange(Vec2 center, double radius, Fn &&fn);

    DeformMesh mesh_;
};

} // namespace ink
