// Magnetic (smart lasso) edge field: gradient build, snapping, thresholding.
#include "core/InkMagnetic.h"
#include "core/InkPaintDevice.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>

using namespace ink;

int main() {
    // A filled square produces a field whose edges attract nearby points.
    {
        PaintDevice dev;
        dev.fillRect({30, 30, 40, 40}, {0, 0, 0, 255});
        MagneticField field;
        field.build(dev, {0, 0, 120, 120});
        INK_CHECK(field.valid());
        INK_CHECK(field.threshold() > 0.0f);

        Vec2 out;
        INK_CHECK(field.snap(25.0, 25.0, 10.0, out));
        INK_CHECK(std::abs(out.x - 30.0) <= 2.0 || std::abs(out.y - 30.0) <= 2.0);
        INK_CHECK(!field.snap(5.0, 100.0, 10.0, out)); // nothing within reach
        INK_CHECK(field.strength(30, 50) > field.strength(60, 60));
        printf("magnetic snap OK threshold=%.1f corner=%.1f,%.1f\n", field.threshold(), out.x, out.y);
    }
    // A blank image has no field to snap to.
    {
        PaintDevice dev;
        MagneticField field;
        field.build(dev, {0, 0, 64, 64});
        INK_CHECK(!field.valid());
        Vec2 out;
        INK_CHECK(!field.snap(32.0, 32.0, 8.0, out));
        printf("magnetic blank OK\n");
    }
    printf("magnetic OK\n");
    return 0;
}
