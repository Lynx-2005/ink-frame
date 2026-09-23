#pragma once
// Tablet sensors + transfer curves. Production: pressure/tilt/velocity with
// per-sensor enable + cubic curve mapping, size/opacity/flow outputs.
#include "brush/InkSmoother.h"
#include <algorithm>
#include <cmath>

namespace ink {

struct TabletState {
    float x{0}, y{0};
    float pressure{0.5f}; // 0..1 (1 = full)
    float tiltX{0}, tiltY{0}; // degrees
    float velocity{0}; // px/sec, filled by stroke interpolator
};

struct SensorCurve {
    // Cubic mapping: y = a*x^3 + b*x^2 + c*x + d shaped by amount/gain
    float amount{1.0f}; // 0 = sensor disabled effect, 1 = full
    bool enabled{true};
    float eval(float x) const {
        if (!enabled) return 1.0f;
        x = std::clamp(x, 0.0f, 1.0f);
        // S-curve favoring control at light touch
        float y = x*x*(3-2*x);
        return 1.0f - amount + amount * (0.15f + 0.85f * y);
    }
};

struct BrushDynamics {
    SensorCurve sizePressure{{1.0f}};
    SensorCurve opacityPressure{{1.0f}};
    bool sizeFromVelocity{false};
    float velocityGamma{0.5f};
    // Freehand path filtering (tool-side smoothing, Krita KisSmoothingOptions
    // concept): stabilizer prefilter + Bezier curve fitting before dab walk.
    SmoothingConfig smoothing;

    float sizeMultiplier(const TabletState &s) const {
        float m = sizePressure.eval(s.pressure);
        if (sizeFromVelocity) {
            float v = std::clamp(s.velocity / 3000.0f, 0.0f, 1.0f);
            m *= 1.0f - velocityGamma * v;
        }
        return std::clamp(m, 0.05f, 2.0f);
    }
    float opacityMultiplier(const TabletState &s) const {
        return std::clamp(opacityPressure.eval(s.pressure), 0.0f, 1.0f);
    }
};

} // namespace ink
