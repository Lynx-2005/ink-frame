#pragma once
// Selection: an 8-bit coverage mask over the document, with boolean combine
// ops, an antialiased polygon rasterizer and a cached marching-ants outline.
// Concept mirrors Krita's pixel-selection + selection actions; clean-room.
#include "core/InkTileStore.h"
#include "core/InkDeform.h"         // DeformMesh
#include "core/InkTransform.h"      // Mat3 / Interpolation
#include "core/InkViewConverter.h"  // Vec2
#include <array>
#include <cstdint>
#include <vector>

namespace ink {

enum class SelectionAction { Replace, Add, Subtract, Intersect };

class Selection {
public:
    void reset(int width, int height); // resize + clear
    void clear();
    void selectAll();
    void invert();

    bool isEmpty() const { return empty_; }
    int width() const { return w_; }
    int height() const { return h_; }
    // Bbox of the RAW mask. The mask lives in the device's pixel space; the
    // animation may DISPLAY those pixels elsewhere (see setDocumentMatrix).
    Rect bounds() const { return bounds_; }
    // Bbox in DOCUMENT space: where the selected ink is displayed right now
    // (the raw bounds pushed through the display matrix).
    Rect docBounds() const;

    // The target layer the mask's ink belongs to (the paint layer id the
    // selection was taken on). The compositor uses it to decide WHICH track's
    // animation drives the ants offset; 0 = unknown (no offset applied).
    void setTargetId(uint64_t id) { targetId_ = id; }
    uint64_t targetId() const { return targetId_; }

    // Axis-aligned boundary runs for the marching-ants overlay, in document
    // space. Cached; rebuilt when the mask changes.
    const std::vector<std::array<float, 4>> &outline();
    void invalidateOutline() { outlineDirty_ = true; }

    // --- animation offset (the selection follows keyframed strokes) --------
    // The mask is stored in the DEVICE's pixel space (lift/erase/brush clip
    // all read it there — the cell pixels are untransformed; the ANIMATION
    // moves them at composite time). setDocumentMatrix tells the selection
    // where the animation currently displays them: outline(), docBounds() and
    // the document-space queries below push through it, so the marching ants
    // sit on the moved stroke and a NEW lasso drawn at the moved position
    // still grabs it — the Krita "outline follows the pixels" contract.
    //
    // It is the FULL matrix, not just its translation: a recorded rotation or
    // perspective skews the ink, and an ants outline offset by the
    // translation alone sits on its own track beside the stroke. (A baked
    // transform, which really does move the cel pixels, resamples the mask
    // instead — resampleMask.)
    //
    // The matrix is deliberately NOT baked into the mask: baking would make
    // the next keyframe start from the last pose.
    void setDocumentMatrix(const Mat3 &m);
    // Convenience for the translation-only case (a plain move key).
    void setDocumentOffset(const Vec2 &off) {
        setDocumentMatrix(Mat3::translation(off.x, off.y));
    }
    const Mat3 &documentMatrix() const { return display_; }
    Vec2 documentOffset() const { return {display_.m[2], display_.m[5]}; }
    bool offsetActive() const { return displayActive_; }
    // Document <-> device mapping for the display matrix (identity when the
    // ink is displayed where it is stored).
    Vec2 toDocumentSpace(const Vec2 &p) const { return displayActive_ ? display_.map(p) : p; }
    Vec2 toDeviceSpace(const Vec2 &p) const { return displayActive_ ? displayInv_.map(p) : p; }

    // coverage/contains: RAW device-space reads. Every pixel pipeline (lift,
    // erase, brush/fill clip) works where the untransformed ink is.
    uint8_t coverage(int x, int y) const { return coverageRaw(x, y); }
    bool contains(int x, int y) const { return coverageRaw(x, y) > 127; }
    uint8_t coverageRaw(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return 0;
        return mask_[size_t(y) * size_t(w_) + size_t(x)];
    }
    // DOCUMENT-space reads (display-aware) for hit-testing and whole-stroke
    // grab logic: the lasso is drawn where the ink is DISPLAYED.
    uint8_t coverageAt(int x, int y) const {
        if (!displayActive_)
            return coverageRaw(x, y);
        const Vec2 d = displayInv_.map({double(x), double(y)});
        return coverageRaw(int(std::floor(d.x)), int(std::floor(d.y)));
    }
    bool containsAt(int x, int y) const { return coverageAt(x, y) > 127; }

    // Combine a polygon (implicitly closed) into the mask.
    void applyPolygon(const std::vector<Vec2> &pts, SelectionAction action, bool antialias = true);
    // Whole-stroke lasso: any ink component (8-connected alpha) touched by the
    // polygon (dilated by gapRadius so a rough circle still grabs the stroke)
    // is selected in full. Falls back to applyPolygon when nothing is touched.
    // `device` is the current layer's cell (current layer only).
    void applyStrokePolygon(const std::vector<Vec2> &pts, SelectionAction action, bool antialias,
                            const class PaintDevice &device, int gapRadius);

    // Whole-mask edits used while dragging a lifted region: the cached outline
    // travels with the mask so a drag does not rescan it.
    void translate(int dx, int dy);
    void mirror(bool horizontal);

    // Resample the mask through a transform: the selection follows the pixels
    // it clips (Krita's transformPixelSelectionOutline). The mesh overload
    // covers the free-form modes, which have no matrix.
    void resampleMask(const Mat3 &forward, Interpolation filter);
    void resampleMask(const DeformMesh &mesh);

private:
    void recomputeBounds();
    void combineMask(const std::vector<uint8_t> &poly, SelectionAction action);
    // Where the animation DISPLAYS the mask's ink (identity when the pixels
    // are shown where they are stored). Outline/docBounds push through it; the
    // document-space queries back-map through its inverse.
    Mat3 display_{};
    Mat3 displayInv_{};
    bool displayActive_{false};
    uint64_t targetId_{0}; // paint layer the mask's ink came from
    static void rasterizePolygon(const std::vector<Vec2> &pts, int w, int h, bool antialias,
                                 std::vector<uint8_t> &out, Rect &outBounds);

    int w_{0}, h_{0};
    std::vector<uint8_t> mask_;
    Rect bounds_{};
    bool empty_{true};
    bool outlineDirty_{true};
    std::vector<std::array<float, 4>> outline_; // x0,y0,x1,y1
};

} // namespace ink
