#pragma once
// Temporal easing: the cubic-bezier curve behind every keyframe segment.
//
// Only the TIME axis lives here. A segment's x-curve is described by two
// control points inside the unit square, which the Web Animations spec and CSS
// cubic-bezier() clamp exactly the same way for one reason: with both x
// control points in [0,1] the curve x(t) is provably monotone, so a segment is
// always a function of time. One value per frame, no folded branches, and a
// root solve that cannot fail.
//
// The VALUE axis is deliberately NOT normalized here. Values are carried in
// absolute channel units by the caller (see ScalarChannel), because a segment
// whose two keys hold the same value has no delta to normalize against, and
// dividing by it would make flat segments degenerate. Absolute tangents stay
// well-defined at every delta, including zero.
#include <algorithm>
#include <cmath>

namespace ink {

// The x-curve of one segment: normalized control points, x in [0,1].
struct TimeCurve {
    double x1{0.3333333333};
    double x2{0.6666666666};

    static TimeCurve make(double cx1, double cx2) {
        TimeCurve c;
        c.x1 = std::clamp(cx1, 0.0, 1.0);
        c.x2 = std::clamp(cx2, 0.0, 1.0);
        return c;
    }
    // True when x(t) is the identity — the solver can then skip straight to
    // x, which is what keeps straight-line motion bit-exact.
    bool isIdentityX(double eps = 1e-9) const {
        return std::abs(x1 - 1.0 / 3.0) < eps && std::abs(x2 - 2.0 / 3.0) < eps;
    }
};

// Bernstein form with free endpoints: the general cubic, used for the value
// axis where p0/p3 are the two key values.
inline double cubicValue(double t, double p0, double p1, double p2, double p3) {
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
}

// 1-D cubic coordinate with implicit 0 and 1 endpoints (the x axis' form).
inline double cubicCoord(double t, double p1, double p2) {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
}

// d/dt of cubicCoord.
inline double cubicDeriv(double t, double p1, double p2) {
    const double u = 1.0 - t;
    return 3.0 * u * u * p1 + 6.0 * u * t * (p2 - p1) + 3.0 * t * t * (1.0 - p2);
}

// Solve x(t) = x for t in [0,1]. Newton-Raphson first (2-4 iterations for any
// sane handle pair), then bisection — which cannot fail, because x is monotone.
double bezierSolveT(double x, const TimeCurve &c, double eps = 1e-7);

// The x-curve that is exactly the identity (control points at 1/3 and 2/3).
extern const TimeCurve kTimeCurveIdentity;

} // namespace ink
