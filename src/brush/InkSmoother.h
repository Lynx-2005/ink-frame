#pragma once
// Freehand smoothing: stabilizer + quadratic midpoint curve flattener.
// Concept mirrors Krita's KisToolFreehand smoothing stage (tool-side path
// filtering before dab placement); all math is clean-room standard technique:
//  - Stabilizer: recency-weighted moving average over recent raw samples
//    (Krita WEIGHTED-mode concept: prefilter by travelled history).
//  - CurveFlattener: quadratic Bezier from previous midpoint to new midpoint
//    with control at the previous raw point, flattened by recursive midpoint
//    subdivision against a flatness threshold (Foley & van Dam style).
//    This turns sparse pointer events into a dense smooth polyline, so fast
//    circles paint as circles instead of polygons.
//  - Dab spacing/pressure interpolation stays in StrokeInterpolator, which
//    walks the flattened path by arc length.
// Pure std C++ — headless unit-testable.
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace ink {

struct RawSample {
    double x{0}, y{0};
    float pressure{0.5f};
    double t{0}; // seconds, monotonic (event timestamp); for speed estimate
};

// Path point with fully-resolved brush multipliers (size/opacity after
// sensor curves), carried through subdivision so dabs lerp smoothly.
struct PathPoint {
    double x{0}, y{0};
    float sizeMul{1.0f};
    float opacityMul{1.0f};
};

inline PathPoint lerpPoint(const PathPoint &a, const PathPoint &b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            float(a.sizeMul + (b.sizeMul - a.sizeMul) * t),
            float(a.opacityMul + (b.opacityMul - a.opacityMul) * t)};
}

enum class SmoothingMode { None, Weighted };

struct SmoothingConfig {
    SmoothingMode mode{SmoothingMode::Weighted};
    float amount{0.35f}; // 0..1 -> maxDistance 2..30px of history influence
    bool smoothPressure{true};
    float maxSpeedPxPerSec{4000.0f}; // speed normalizer for sigma adaptation
};

class Stabilizer {
public:
    explicit Stabilizer(SmoothingConfig cfg = {}) : cfg_(cfg) {}
    void setConfig(SmoothingConfig c) {
        cfg_ = c;
        hist_.clear();
        speed_ = 0;
        haveSpeed_ = false;
    }
    void reset(const RawSample &s) {
        hist_.clear();
        hist_.push_back(s);
        speed_ = 0;
        haveSpeed_ = false;
    }
    // Returns the stabilized sample for this raw input.
    // Speed-adaptive Gaussian prefilter in distance space (Krita WEIGHTED
    // concept): sigma shrinks as pointer speed rises, so slow detail work
    // gets ironed while fast curves (circles) pass through un-shrunk.
    RawSample push(const RawSample &s) {
        hist_.push_back(s);
        while (hist_.size() > 64) hist_.pop_front();
        const double maxD = maxDistance();
        if (cfg_.mode == SmoothingMode::None || maxD <= 0.01 || hist_.size() < 2)
            return s;
        // Instantaneous speed of the newest segment, lightly smoothed.
        const RawSample &b = hist_.back();
        const RawSample &a = hist_[hist_.size() - 2];
        double dt = b.t - a.t;
        if (!(dt > 0)) dt = 1e-4;
        double inst = std::hypot(b.x - a.x, b.y - a.y) / dt;
        if (!haveSpeed_) {
            speed_ = inst;
            haveSpeed_ = true;
        } else {
            speed_ += (inst - speed_) * 0.3;
        }
        double speedN = std::clamp(speed_ / std::max(1.0f, cfg_.maxSpeedPxPerSec), 0.0, 1.0);
        double sigma = std::max(0.5, maxD * (1.0 - 0.75 * speedN));
        // Walk back over travelled distance with Gaussian falloff; stop past
        // 3 sigma (weight < 1%). Newest sample always anchors (weight 1).
        double wsum = 0, sx = 0, sy = 0, sp = 0, acc = 0;
        const size_t n = hist_.size();
        for (size_t k = 0; k < n; ++k) {
            size_t i = n - 1 - k;
            if (k > 0) {
                acc += std::hypot(hist_[i + 1].x - hist_[i].x, hist_[i + 1].y - hist_[i].y);
                if (acc > 3.0 * sigma) break;
            }
            double w = std::exp(-(acc * acc) / (2.0 * sigma * sigma));
            wsum += w;
            sx += w * hist_[i].x;
            sy += w * hist_[i].y;
            sp += w * hist_[i].pressure;
        }
        if (wsum <= 0) return s;
        return {sx / wsum, sy / wsum, cfg_.smoothPressure ? float(sp / wsum) : s.pressure, s.t};
    }

private:
    double maxDistance() const {
        if (cfg_.mode == SmoothingMode::None) return 0;
        return 2.0 + std::clamp(cfg_.amount, 0.0f, 1.0f) * 28.0; // 2..30 px
    }
    SmoothingConfig cfg_;
    std::deque<RawSample> hist_;
    double speed_{0};
    bool haveSpeed_{false};
};

class CurveFlattener {
public:
    explicit CurveFlattener(double flatness = 0.25) : flatness_(flatness) {}
    void reset(const PathPoint &p) {
        prev_ = p;
        prevMid_ = p;
        started_ = true;
    }
    bool started() const { return started_; }
    // Feed a stabilized point; returns dense segment points, excluding the
    // segment start (already emitted) and including the segment end.
    std::vector<PathPoint> push(const PathPoint &s) {
        std::vector<PathPoint> out;
        if (!started_) {
            reset(s);
            out.push_back(s);
            return out;
        }
        PathPoint mid{ (prev_.x + s.x) * 0.5, (prev_.y + s.y) * 0.5,
                       (prev_.sizeMul + s.sizeMul) * 0.5f,
                       (prev_.opacityMul + s.opacityMul) * 0.5f };
        auto seg = flattenQuad(prevMid_, prev_, mid, flatness_);
        out.insert(out.end(), seg.begin(), seg.end());
        prevMid_ = mid;
        prev_ = s;
        return out;
    }
    // Stroke end: pull the tail to the final raw point.
    std::vector<PathPoint> finish() {
        std::vector<PathPoint> out;
        if (!started_) return out;
        auto seg = flattenQuad(prevMid_, prev_, prev_, flatness_);
        out.insert(out.end(), seg.begin(), seg.end());
        started_ = false;
        return out;
    }
    // Quadratic Bezier p0 -> p2 (control p1) flattened to polyline.
    // Includes end point, excludes start point.
    static std::vector<PathPoint> flattenQuad(const PathPoint &p0, const PathPoint &p1,
                                              const PathPoint &p2, double flatness) {
        std::vector<PathPoint> out;
        flattenRec(p0, p1, p2, flatness, 0, out);
        return out;
    }

private:
    static void flattenRec(const PathPoint &p0, const PathPoint &p1, const PathPoint &p2,
                           double flatness, int depth, std::vector<PathPoint> &out) {
        // Distance of control point from chord p0-p2 (flatness metric).
        double dx = p2.x - p0.x, dy = p2.y - p0.y;
        double len = std::sqrt(dx * dx + dy * dy);
        double dist = 0;
        if (len > 1e-9) dist = std::abs((p1.x - p0.x) * dy - (p1.y - p0.y) * dx) / len;
        else dist = std::hypot(p1.x - p0.x, p1.y - p0.y);
        if (dist <= flatness || depth >= 12 || len < 1e-6) {
            out.push_back(p2);
            return;
        }
        // de Casteljau split at t=0.5 (all channels incl. multipliers).
        PathPoint a = lerpPoint(p0, p1, 0.5);
        PathPoint b = lerpPoint(p1, p2, 0.5);
        PathPoint m = lerpPoint(a, b, 0.5);
        flattenRec(p0, a, m, flatness, depth + 1, out);
        flattenRec(m, b, p2, flatness, depth + 1, out);
    }
    double flatness_;
    bool started_{false};
    PathPoint prev_{}, prevMid_{};
};

} // namespace ink
