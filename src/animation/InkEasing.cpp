#include "animation/InkEasing.h"

namespace ink {

const TimeCurve kTimeCurveIdentity = TimeCurve::make(1.0 / 3.0, 2.0 / 3.0);

double bezierSolveT(double x, const TimeCurve &c, double eps) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    if (c.isIdentityX()) return x; // no solve needed for the identity curve
    // Newton-Raphson: the cheap path that carries almost every real handle.
    double t = x;
    for (int i = 0; i < 8; ++i) {
        const double ex = cubicCoord(t, c.x1, c.x2) - x;
        if (std::abs(ex) < eps) return t;
        const double d = cubicDeriv(t, c.x1, c.x2);
        if (std::abs(d) < 1e-9) break; // flat tangent: Newton has nothing to say
        const double nt = t - ex / d;
        if (!(nt > 0.0 && nt < 1.0)) break; // stepped outside the bracket
        t = nt;
    }
    // Bisection on [0,1]. Since x1 and x2 are clamped, x(t) is monotone and the
    // root is unique — this always converges.
    double lo = 0.0, hi = 1.0;
    t = x;
    for (int i = 0; i < 48; ++i) {
        const double xa = cubicCoord(t, c.x1, c.x2);
        if (std::abs(xa - x) < eps) break;
        if (xa < x) lo = t;
        else hi = t;
        t = 0.5 * (lo + hi);
    }
    return t;
}

} // namespace ink
