#pragma once
// Brush tip: procedural auto (circle) mask generator with hardness/softness,
// spaced stamping, size/opacity jitter. Production: precomputed pyramid of
// masks per size bucket + sub-pixel centered sampling.
#include <algorithm>
#include <cmath>
#include <vector>

namespace ink {

struct BrushTipShape {
    float diameter{32.0f}; // px
    float hardness{0.75f}; // 0 soft .. 1 hard
    float opacity{1.0f};
    float spacing{0.08f};  // fraction of diameter
};

class AutoBrushTip {
public:
    explicit AutoBrushTip(BrushTipShape shape = {}) : shape_(shape) { rebuild(); }
    void setShape(BrushTipShape s) { shape_ = s; rebuild(); }
    const BrushTipShape &shape() const { return shape_; }
    // Odd-sized square mask in [0,1]
    const std::vector<float> &mask() const { return mask_; }
    int maskSize() const { return size_; }
    float spacingPx() const { return std::max(1.0f, shape_.diameter * shape_.spacing); }
private:
    void rebuild();
    BrushTipShape shape_;
    std::vector<float> mask_;
    int size_{1};
};

inline void AutoBrushTip::rebuild() {
    float d = std::max(1.0f, shape_.diameter);
    size_ = static_cast<int>(std::ceil(d)) | 1; // odd
    if (size_ < 3) size_ = 3;
    if (size_ > 513) size_ = 513;
    mask_.assign(size_*size_, 0.0f);
    float r = d * 0.5f;
    float hardR = r * std::clamp(shape_.hardness, 0.0f, 1.0f);
    float c = (size_-1) * 0.5f;
    for (int y = 0; y < size_; ++y)
        for (int x = 0; x < size_; ++x) {
            float dx = x - c, dy = y - c;
            float dist = std::sqrt(dx*dx + dy*dy);
            float a = 0.0f;
            if (dist <= hardR) a = 1.0f;
            else if (dist < r && r > hardR) {
                float t = (dist - hardR) / (r - hardR);
                a = 1.0f - t*t*(3-2*t); // smoothstep falloff
            }
            mask_[y*size_+x] = a;
        }
}

} // namespace ink
