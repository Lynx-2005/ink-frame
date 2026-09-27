// Temporal easing: the cubic-bezier x-solver and the absolute-value cubic.
// These are the primitives every keyframe segment evaluates through, so the
// proofs here are about guarantees, not about pretty curves:
//   - x(t) is monotone for ANY legal handle pair (so a segment is a function of
//     time and the solver cannot pick the wrong branch),
//   - the identity curve and the straight line are exact, not approximate,
//   - the solver converges where plain Newton would stall.
#include "animation/InkEasing.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>

using namespace ink;

namespace {

constexpr double kTol = 1e-6;

bool near(double a, double b, double tol = kTol) { return std::abs(a - b) <= tol; }

// The x-curve never decreases: sample hard, including at the extremes.
bool monotone(const TimeCurve &c) {
    double prev = -1.0;
    for (int i = 0; i <= 1000; ++i) {
        const double t = double(i) / 1000.0;
        const double x = cubicCoord(t, c.x1, c.x2);
        if (x < prev - 1e-12) return false;
        prev = x;
    }
    return true;
}

} // namespace

int main() {
    // 1. Endpoints and the identity curve are exact.
    {
        INK_CHECK(cubicCoord(0.0, 0.3, 0.7) == 0.0);
        INK_CHECK(cubicCoord(1.0, 0.3, 0.7) == 1.0);
        INK_CHECK(kTimeCurveIdentity.isIdentityX());
        INK_CHECK(near(bezierSolveT(0.0, kTimeCurveIdentity), 0.0));
        INK_CHECK(near(bezierSolveT(1.0, kTimeCurveIdentity), 1.0));
        INK_CHECK(near(bezierSolveT(0.42, kTimeCurveIdentity), 0.42));
        INK_CHECK(bezierSolveT(-5.0, kTimeCurveIdentity) == 0.0);
        INK_CHECK(bezierSolveT(5.0, kTimeCurveIdentity) == 1.0);
        std::printf("endpoints + identity OK\n");
    }

    // 2. The solver inverts x(t) to solver tolerance across the whole domain,
    //    for easy-ease handles and for the extreme pairs that break Newton.
    {
        const TimeCurve curves[] = {
            TimeCurve::make(1.0 / 3.0, 2.0 / 3.0),
            TimeCurve::make(0.0, 1.0),          // full ramp-in
            TimeCurve::make(1.0, 0.0),          // full ramp-out
            TimeCurve::make(0.6, 0.6),          // near-stationary midpoint
            TimeCurve::make(0.0, 0.0),          // cubic-shaped x
            TimeCurve::make(1.0, 1.0),
            TimeCurve::make(0.999, 0.001),      // handles that cross
        };
        for (const TimeCurve &c : curves) {
            INK_CHECK(monotone(c));
            for (int i = 1; i < 200; ++i) {
                const double x = double(i) / 200.0;
                const double t = bezierSolveT(x, c);
                INK_CHECK(t >= 0.0 && t <= 1.0);
                INK_CHECK(near(cubicCoord(t, c.x1, c.x2), x, 1e-6));
            }
        }
        std::printf("solver inversion OK\n");
    }

    // 3. Clamping happens on construction: x stays inside [0,1] whatever the
    //    caller passes, which is what makes monotonicity a guarantee.
    {
        const TimeCurve lo = TimeCurve::make(-3.0, 9.0);
        const TimeCurve hi = TimeCurve::make(4.0, -2.0);
        INK_CHECK(lo.x1 == 0.0 && lo.x2 == 1.0);
        INK_CHECK(hi.x1 == 1.0 && hi.x2 == 0.0);
        INK_CHECK(monotone(hi));
        std::printf("clamp + monotonicity OK\n");
    }

    // 4. cubicValue with free endpoints: exact at both ends, symmetric about the
    //    midpoint for mirrored control points.
    {
        INK_CHECK(near(cubicValue(0.0, 5.0, 9.0, 3.0, -2.0), 5.0));
        INK_CHECK(near(cubicValue(1.0, 5.0, 9.0, 3.0, -2.0), -2.0));
        // A straight line in disguise: control points on the chord at 1/3 and
        // 2/3 reproduce the chord exactly. This is the property that lets an
        // auto-tangent key land exactly on linear motion.
        const double v0 = 10.0, v1 = 40.0, d = v1 - v0;
        for (int i = 0; i <= 10; ++i) {
            const double t = double(i) / 10.0;
            const double v = cubicValue(t, v0, v0 + d / 3.0, v1 - d / 3.0, v1);
            INK_CHECK(near(v, v0 + t * d, 1e-9));
        }
        std::printf("free-endpoint cubic OK\n");
    }

    // 5. Derivative: matches a numerical difference of the curve.
    {
        const TimeCurve c = TimeCurve::make(0.25, 0.75);
        for (double t : {0.1, 0.35, 0.5, 0.8}) {
            const double h = 1e-6;
            const double num =
                (cubicValue(t + h, 0.0, c.x1, c.x2, 1.0) - cubicValue(t - h, 0.0, c.x1, c.x2, 1.0)) /
                (2 * h);
            INK_CHECK(near(cubicDeriv(t, c.x1, c.x2), num, 1e-5));
        }
        std::printf("derivative OK\n");
    }

    std::printf("test_easing PASS\n");
    return 0;
}
