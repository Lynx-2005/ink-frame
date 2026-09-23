#pragma once
// Selection: an 8-bit coverage mask over the document, with boolean combine
// ops, an antialiased polygon rasterizer and a cached marching-ants outline.
// Concept mirrors Krita's pixel-selection + selection actions; clean-room.
#include "core/InkTileStore.h"
#include "core/InkViewConverter.h" // Vec2
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
    Rect bounds() const { return bounds_; } // bbox of selected pixels

    uint8_t coverage(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return 0;
        return mask_[size_t(y) * size_t(w_) + size_t(x)];
    }
    bool contains(int x, int y) const { return coverage(x, y) > 127; }

    // Combine a polygon (implicitly closed) into the mask.
    void applyPolygon(const std::vector<Vec2> &pts, SelectionAction action, bool antialias = true);

    // Axis-aligned boundary runs for the marching-ants overlay, in document
    // space. Cached; rebuilt when the mask changes.
    const std::vector<std::array<float, 4>> &outline();

private:
    void recomputeBounds();
    void combineMask(const std::vector<uint8_t> &poly, SelectionAction action);
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
