#pragma once
// Edge field for the smart lasso: a Sobel gradient magnitude over the drawing,
// plus a local search that pulls a pointer position onto the nearest strong
// edge. Clean-room take on the magnetic-selection idea (Krita's
// KisMagneticWorker) without any of its code.
#include "core/InkPaintDevice.h"
#include "core/InkTileStore.h"
#include "core/InkViewConverter.h" // Vec2
#include <vector>

namespace ink {

class MagneticField {
public:
    // Sample `source` (usually the merged projection) over `region`.
    void build(const PaintDevice &source, const Rect &region);
    bool valid() const { return valid_; }
    Rect region() const { return region_; }
    float threshold() const { return threshold_; }
    float strength(int x, int y) const;

    // Strongest edge pixel within `radius` of (x, y), biased towards the
    // cursor. False when nothing above the threshold is that close.
    bool snap(double x, double y, double radius, Vec2 &out) const;

private:
    Rect region_{};
    std::vector<float> mag_;
    float threshold_{0.0f};
    bool valid_{false};
};

} // namespace ink
