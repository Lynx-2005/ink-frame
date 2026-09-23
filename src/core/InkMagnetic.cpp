#include "core/InkMagnetic.h"
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

// Backdrop used to give transparent pixels a value: dark strokes on an empty
// canvas must still read as edges.
constexpr float kBackdrop = 214.0f;

inline float luma(int r, int g, int b) { return 0.299f * float(r) + 0.587f * float(g) + 0.114f * float(b); }

} // namespace

void MagneticField::build(const PaintDevice &source, const Rect &region) {
    valid_ = false;
    mag_.clear();
    region_ = region;
    if (region.isEmpty())
        return;

    const int w = region.w, h = region.h;
    std::vector<uint8_t> buf(size_t(w) * size_t(h) * 4, 0);
    source.readRect(region, buf.data());

    // Two planes: the pixel colour composited over the empty-canvas backdrop
    // (so a black stroke on transparency still has contrast) and the alpha
    // itself (so a white stroke on transparency does too). An edge counts when
    // either plane changes.
    std::vector<float> lum(size_t(w) * size_t(h), 0.0f);
    std::vector<float> alpha(size_t(w) * size_t(h), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const int a = buf[i * 4 + 3];
            const float cov = float(a) / 255.0f;
            lum[i] = luma(buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]) * cov +
                     kBackdrop * (1.0f - cov);
            alpha[i] = float(a);
        }
    }

    mag_.assign(size_t(w) * size_t(h), 0.0f);
    float maxMag = 0.0f;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float gx = -lum[i - w - 1] - 2.0f * lum[i - 1] - lum[i + w - 1] +
                             lum[i - w + 1] + 2.0f * lum[i + 1] + lum[i + w + 1];
            const float gy = -lum[i - w - 1] - 2.0f * lum[i - w] - lum[i - w + 1] +
                             lum[i + w - 1] + 2.0f * lum[i + w] + lum[i + w + 1];
            const float agx = -alpha[i - w - 1] - 2.0f * alpha[i - 1] - alpha[i + w - 1] +
                              alpha[i - w + 1] + 2.0f * alpha[i + 1] + alpha[i + w + 1];
            const float agy = -alpha[i - w - 1] - 2.0f * alpha[i - w] - alpha[i - w + 1] +
                              alpha[i + w - 1] + 2.0f * alpha[i + w] + alpha[i + w + 1];
            const float m = std::max(std::sqrt(gx * gx + gy * gy),
                                     0.7f * std::sqrt(agx * agx + agy * agy));
            mag_[i] = m;
            maxMag = std::max(maxMag, m);
        }
    }
    // Adaptive floor so faint anti-aliased edges still attract, while an
    // almost-flat image produces no snapping at all.
    threshold_ = std::max(24.0f, 0.16f * maxMag);
    valid_ = maxMag > 1.0f;
}

float MagneticField::strength(int x, int y) const {
    if (!valid_)
        return 0.0f;
    const int lx = x - region_.x, ly = y - region_.y;
    if (lx < 0 || ly < 0 || lx >= region_.w || ly >= region_.h)
        return 0.0f;
    return mag_[size_t(ly) * size_t(region_.w) + size_t(lx)];
}

bool MagneticField::snap(double x, double y, double radius, Vec2 &out) const {
    if (!valid_ || radius < 0.5)
        return false;
    const int r = int(std::ceil(radius));
    const int cx = int(std::lround(x)), cy = int(std::lround(y));
    float best = 0.0f;
    int bx = 0, by = 0;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const double d = std::sqrt(double(dx * dx + dy * dy));
            if (d > radius)
                continue;
            const int px = cx + dx, py = cy + dy;
            const float m = strength(px, py);
            if (m < threshold_)
                continue;
            // Prefer strong edges that are close to the cursor.
            const float score = m * float(1.0 - 0.4 * (d / radius));
            if (score > best) {
                best = score;
                bx = px;
                by = py;
            }
        }
    }
    if (best <= 0.0f)
        return false;
    out = {double(bx) + 0.5, double(by) + 0.5};
    return true;
}

} // namespace ink
