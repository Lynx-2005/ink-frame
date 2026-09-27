// Parameter channels: the keyframed transform an adjustment layer owns.
// The load-bearing proofs here:
//   - Linear and Hold behave EXACTLY as the pre-keyframe ScalarChannel did, so
//     existing projects and the opacity channel are untouched,
//   - a key returns its own stored value (never a solver-drifted one),
//   - an auto-tangent chain is exactly linear (a recorded drag must not wobble),
//   - an empty block is the identity map (the no-regression contract).
#include "core/InkParams.h"
#include "test_check.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ink;

namespace {
bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }
} // namespace

int main() {
    // 1. Hold and Linear keep the pre-existing semantics, bit for bit.
    {
        ScalarChannel ch;
        ch.setKey(0, 0.0f, Interp::Linear);
        ch.setKey(10, 10.0f, Interp::Linear);
        INK_CHECK(ch.valueAt(0) == 0.0f);
        INK_CHECK(ch.valueAt(5) == 5.0f);
        INK_CHECK(ch.valueAt(10) == 10.0f);
        INK_CHECK(ch.valueAt(-3) == 0.0f);   // before the first key holds
        INK_CHECK(ch.valueAt(99) == 10.0f);  // past the last key holds
        INK_CHECK(ch.valueAt(3, 0.25f) == 3.0f);
        ScalarChannel hold;
        hold.setKey(2, 1.5f, Interp::Hold);
        hold.setKey(6, 4.0f, Interp::Hold);
        INK_CHECK(hold.valueAt(5) == 1.5f);
        INK_CHECK(hold.valueAt(6) == 4.0f);
        ScalarChannel empty;
        INK_CHECK(empty.valueAt(3, 0.7f) == 0.7f);
        std::printf("hold/linear/empty OK\n");
    }

    // 2. A key always evaluates to exactly its own value, at integer and
    //    fractional frames, in every interpolation mode.
    {
        for (Interp mode : {Interp::Hold, Interp::Linear, Interp::Bezier}) {
            ScalarChannel ch;
            ch.setKey(4, 12.5f, mode);
            ch.setKey(9, -3.25f, mode);
            ch.setKey(20, 7.0f, mode);
            for (int f : {4, 9, 20}) {
                const ScalarKey *k = ch.keyAt(f);
                INK_CHECK(k != nullptr);
                INK_CHECK(ch.valueAtF(double(f)) == double(k->value));
                INK_CHECK(ch.valueAtF(double(f) - 0.0) == double(k->value));
            }
        }
        std::printf("exact key values OK\n");
    }

    // 3. An auto-tangent chain is a straight line: neighbouring keys collinear
    //    in value must interpolate exactly (the "no wobble" guarantee that makes
    //    a recorded drag honest).
    {
        ParamBlock b;
        for (int i = 0; i <= 4; ++i)
            b.setKey(ParamId::PositionX, i * 10, double(i) * 25.0, Interp::Bezier);
        for (int f = 0; f <= 40; ++f) {
            INK_CHECK(near(b.valueAt(ParamId::PositionX, double(f)), double(f) * 2.5, 1e-3));
        }
        std::printf("auto tangent is linear OK\n");
    }

    // 4. Easy Ease (speed 0 both sides) is a real ease: it starts and ends
    //    slower than the chord, and passes the midpoint at the midpoint.
    {
        ParamBlock b;
        b.setKey(ParamId::PositionY, 0, 0.0, Interp::Linear);
        b.setKey(ParamId::PositionY, 10, 100.0, Interp::Linear);
        INK_CHECK(near(b.valueAt(ParamId::PositionY, 5.0), 50.0, 1e-3));
        // now make both ends bezier with zero speed
        b.setKey(ParamId::PositionY, 0, 0.0, Interp::Bezier);
        b.setKey(ParamId::PositionY, 10, 100.0, Interp::Bezier);
        b.setHandles(ParamId::PositionY, 0, 1.0 / 3.0, 0.0, 1.0 / 3.0, 0.0);
        b.setHandles(ParamId::PositionY, 10, 1.0 / 3.0, 0.0, 1.0 / 3.0, 0.0);
        INK_CHECK(near(b.valueAt(ParamId::PositionY, 5.0), 50.0, 1e-3));
        INK_CHECK(b.valueAt(ParamId::PositionY, 1.0) < 10.0);   // slower than chord
        INK_CHECK(b.valueAt(ParamId::PositionY, 9.0) > 90.0);   // slower than chord
        // Back to Linear: the same two keys become the exact chord again, which
        // is what makes the Linear preset safe to toggle.
        b.setInterp(ParamId::PositionY, 0, Interp::Linear);
        b.setInterp(ParamId::PositionY, 10, Interp::Linear);
        INK_CHECK(near(b.valueAt(ParamId::PositionY, 1.0), 10.0, 1e-4));
        std::printf("bezier ease OK\n");
    }

    // 5. A flat segment with bent handles still evaluates: no division by the
    //    (zero) value delta anywhere.
    {
        ParamBlock b;
        b.setKey(ParamId::ScaleX, 0, 2.0, Interp::Bezier);
        b.setKey(ParamId::ScaleX, 8, 2.0, Interp::Bezier);
        for (int f = 0; f <= 8; ++f) {
            const double v = b.valueAt(ParamId::ScaleX, double(f));
            INK_CHECK(std::isfinite(v));
        }
        INK_CHECK(near(b.valueAt(ParamId::ScaleX, 0.0), 2.0));
        INK_CHECK(near(b.valueAt(ParamId::ScaleX, 8.0), 2.0));
        std::printf("zero-delta segment OK\n");
    }

    // 6. Stopwatch semantics: an un-keyed parameter reads its static value; the
    //    first key is the value it already had, so nothing jumps.
    {
        ParamBlock b;
        INK_CHECK(!b.isAnimated(ParamId::Rotation));
        INK_CHECK(b.valueAt(ParamId::Rotation, 3.0) == paramDesc(ParamId::Rotation).def);
        b.setStaticValue(ParamId::Rotation, 30.0);
        INK_CHECK(b.valueAt(ParamId::Rotation, 3.0) == 30.0);
        b.setKey(ParamId::Rotation, 5, b.valueAt(ParamId::Rotation, 5.0), Interp::Bezier);
        INK_CHECK(b.isAnimated(ParamId::Rotation));
        INK_CHECK(b.valueAt(ParamId::Rotation, 5.0) == 30.0);
        INK_CHECK(b.valueAt(ParamId::Rotation, 99.0) == 30.0); // holds past the key
        std::printf("stopwatch semantics OK\n");
    }

    // 7. Key bookkeeping: add / update / remove / move, plus the neighbour
    //    queries the timeline and the editor navigate with.
    {
        ParamBlock b;
        INK_CHECK(!b.removeKey(ParamId::Opacity, 3));
        b.setKey(ParamId::Opacity, 3, 1.0);
        b.setKey(ParamId::Opacity, 9, 0.5);
        INK_CHECK(b.hasKey(ParamId::Opacity, 3));
        INK_CHECK(b.keys(ParamId::Opacity).size() == 2);
        INK_CHECK(b.firstKey(ParamId::Opacity) == 3);
        INK_CHECK(b.lastKey(ParamId::Opacity) == 9);
        INK_CHECK(b.nextKey(ParamId::Opacity, 3) == 9);
        INK_CHECK(b.nextKey(ParamId::Opacity, 9) == -1);
        INK_CHECK(b.prevKey(ParamId::Opacity, 9) == 3);
        INK_CHECK(b.prevKey(ParamId::Opacity, 3) == -1);
        INK_CHECK(b.hasAnyKeyAt(9));
        INK_CHECK(b.keysAt(9).size() == 1);
        INK_CHECK(!b.setKey(ParamId::Opacity, 3, 0.25, Interp::Bezier, /*updateExisting=*/false));
        INK_CHECK(b.valueAt(ParamId::Opacity, 3.0) == 1.0);
        INK_CHECK(b.setKeyValue(ParamId::Opacity, 3, 0.25));
        INK_CHECK(b.valueAt(ParamId::Opacity, 3.0) == 0.25);
        INK_CHECK(b.moveKey(ParamId::Opacity, 3, 5));
        INK_CHECK(b.hasKey(ParamId::Opacity, 5) && !b.hasKey(ParamId::Opacity, 3));
        INK_CHECK(b.removeKey(ParamId::Opacity, 5));
        INK_CHECK(b.keys(ParamId::Opacity).size() == 1);
        // removeKeysAt drops every parameter keyed at that frame at once.
        b.setKey(ParamId::PositionX, 4, 1.0);
        b.setKey(ParamId::PositionY, 4, 2.0);
        INK_CHECK(b.removeKeysAt(4) == 2);
        INK_CHECK(b.keys(ParamId::PositionX).empty());
        INK_CHECK(b.removeKeysAt(9) == 1);
        INK_CHECK(b.keyCount() == 0);
        INK_CHECK(b.empty());
        std::printf("key bookkeeping OK\n");
    }

    // 8. Empty block = identity map and full opacity. This is the regression
    //    contract that keeps every existing project pixel-identical.
    {
        ParamBlock b;
        float op = 0.0f;
        const Mat3 m = b.matrixFor(Rect{10, 20, 100, 50}, 7.0, &op);
        INK_CHECK(m.isIdentity());
        INK_CHECK(op == 1.0f);
        INK_CHECK(b.empty());
        // A static value equal to the default is still "empty": writing the same
        // number back must not flip a layer into the resampling path.
        b.setStaticValue(ParamId::PositionX, paramDesc(ParamId::PositionX).def);
        INK_CHECK(b.empty());
        b.setStaticValue(ParamId::PositionX, 12.0);
        INK_CHECK(!b.empty());
        INK_CHECK(!b.matrixFor(Rect{10, 20, 100, 50}, 7.0).isIdentity());
        std::printf("empty block identity OK\n");
    }

    // 9. matrixFor composes exactly like FreeTransformParams::matrix(): same
    //    pivot/scale/rotate/shear order and the same projective override rule.
    {
        const Rect src{100, 60, 200, 120};
        const Vec2 pivot{src.x + 0.5 * src.w, src.y + 0.5 * src.h}; // anchor 0.5,0.5

        FreeTransformParams fp;
        fp.reset();
        fp.pivot = pivot;
        fp.translate = {13.0, -7.0};
        fp.scaleX = 1.4;
        fp.scaleY = 0.8;
        fp.angleDeg = 27.0;
        fp.shearX = 0.15;
        fp.shearY = -0.05;

        ParamBlock b;
        b.setStaticValue(ParamId::AnchorX, 0.5);
        b.setStaticValue(ParamId::AnchorY, 0.5);
        b.setStaticValue(ParamId::PositionX, 13.0);
        b.setStaticValue(ParamId::PositionY, -7.0);
        b.setStaticValue(ParamId::ScaleX, 1.4);
        b.setStaticValue(ParamId::ScaleY, 0.8);
        b.setStaticValue(ParamId::Rotation, 27.0);
        b.setStaticValue(ParamId::ShearX, 0.15);
        b.setStaticValue(ParamId::ShearY, -0.05);

        const Mat3 a = fp.matrix(src);
        const Mat3 c = b.matrixFor(src, 0.0);
        for (int i = 0; i < 9; ++i) INK_CHECK(near(a.m[i], c.m[i], 1e-12));
        // and the mapped corners agree
        const Vec2 probe{150.0, 90.0};
        const Vec2 pa = a.map(probe), pc = c.map(probe);
        INK_CHECK(near(pa.x, pc.x, 1e-9) && near(pa.y, pc.y, 1e-9));

        // Perspective override: the same four corners must produce the same map.
        Vec2 quad[4] = {{90.0, 40.0}, {330.0, 70.0}, {300.0, 200.0}, {110.0, 175.0}};
        fp.hasQuad = true;
        for (int i = 0; i < 4; ++i) fp.quadSrc[i] = quad[i];
        // quadSrc is in absolute document space; the params are normalized to
        // the source rect.
        b.setStaticValue(ParamId::Corner0X, (quad[0].x - src.x) / src.w);
        b.setStaticValue(ParamId::Corner0Y, (quad[0].y - src.y) / src.h);
        b.setStaticValue(ParamId::Corner1X, (quad[1].x - src.x) / src.w);
        b.setStaticValue(ParamId::Corner1Y, (quad[1].y - src.y) / src.h);
        b.setStaticValue(ParamId::Corner2X, (quad[2].x - src.x) / src.w);
        b.setStaticValue(ParamId::Corner2Y, (quad[2].y - src.y) / src.h);
        b.setStaticValue(ParamId::Corner3X, (quad[3].x - src.x) / src.w);
        b.setStaticValue(ParamId::Corner3Y, (quad[3].y - src.y) / src.h);
        const Mat3 a2 = fp.matrix(src);
        const Mat3 c2 = b.matrixFor(src, 0.0);
        for (int i = 0; i < 9; ++i) INK_CHECK(near(a2.m[i], c2.m[i], 1e-9));
        std::printf("free-transform parity OK\n");
    }

    // 10. Opacity clamps to [0,1] and rides along with the matrix.
    {
        ParamBlock b;
        b.setStaticValue(ParamId::Opacity, 0.4);
        float op = 1.0f;
        b.matrixFor(Rect{0, 0, 10, 10}, 0.0, &op);
        INK_CHECK(near(double(op), 0.4));
        b.setStaticValue(ParamId::Opacity, 3.0);
        b.matrixFor(Rect{0, 0, 10, 10}, 0.0, &op);
        INK_CHECK(op == 1.0f);
        std::printf("opacity clamp OK\n");
    }

    // 11. shift/mirror move keys without dropping the curve they carry.
    {
        ParamBlock b;
        b.setKey(ParamId::PositionX, 5, 1.0);
        b.setKey(ParamId::PositionX, 9, 2.0);
        b.shiftKeys(5, 3, 999);
        INK_CHECK(b.keys(ParamId::PositionX) == (std::vector<int>{8, 12}));
        ParamBlock m;
        m.setKey(ParamId::ScaleX, 2, 1.0);
        m.setKey(ParamId::ScaleX, 6, 2.0);
        INK_CHECK(m.mirrorRange(0, 8) == 2);
        INK_CHECK(m.keys(ParamId::ScaleX) == (std::vector<int>{2, 6}));
        std::printf("shift/mirror OK\n");
    }

    // 12. Auto vs free handles. The case that matters is recording: the first
    //     key of a drag has no neighbours yet, and the second key must make the
    //     pair read as ONE straight move rather than an S-curve.
    {
        ParamBlock b;
        b.setKey(ParamId::PositionX, 10, 100.0);
        const ScalarChannel *ch = b.channel(ParamId::PositionX);
        INK_CHECK(ch->autoBez(10));
        INK_CHECK(near(double(ch->keyAt(10)->outSpeed), 0.0));
        INK_CHECK(near(double(ch->keyAt(10)->inSpeed), 0.0));
        b.setKey(ParamId::PositionX, 20, 200.0);
        INK_CHECK(near(double(ch->keyAt(10)->outSpeed), 10.0, 1e-4));
        INK_CHECK(near(b.valueAt(ParamId::PositionX, 15.0), 150.0, 1e-3));
        // A key inserted BETWEEN two others takes the two-sided chord, and the
        // keys it lands between re-derive from it.
        ParamBlock c;
        c.setKey(ParamId::PositionX, 0, 0.0);
        c.setKey(ParamId::PositionX, 20, 40.0);
        c.setKey(ParamId::PositionX, 10, 10.0);
        const ScalarChannel *cc = c.channel(ParamId::PositionX);
        INK_CHECK(near(double(cc->keyAt(10)->outSpeed), 2.0, 1e-4)); // (40-0)/20
        INK_CHECK(near(double(cc->keyAt(0)->outSpeed), 1.0, 1e-4));  // (10-0)/10
        INK_CHECK(near(double(cc->keyAt(20)->outSpeed), 3.0, 1e-4)); // one-sided
        // Hand-set handles are FREE: later refreshes leave them alone.
        c.setHandles(ParamId::PositionX, 10, 0.5, 4.0, 0.5, 4.0);
        INK_CHECK(!cc->autoBez(10));
        c.setKey(ParamId::PositionX, 5, 5.0);
        c.refreshAutoHandles(ParamId::PositionX);
        INK_CHECK(near(double(cc->keyAt(10)->outSpeed), 4.0));
        std::printf("auto vs free handles OK\n");
    }

    // isIdentityAt: the compositor's per-frame "does this adjustment do
    // anything here?" gate. Keys EXISTING is not the question — the evaluated
    // value is, so a curve passing through rest still costs nothing at the
    // crossing.
    {
        ParamBlock b;
        INK_CHECK(b.isIdentityAt(0));
        INK_CHECK(b.isIdentityAt(42.5));
        b.setStaticValue(ParamId::PositionX, 5.0);
        INK_CHECK(!b.isIdentityAt(0));
        b.setStaticValue(ParamId::PositionX, 0.0); // back to default explicitly
        INK_CHECK(b.isIdentityAt(0));

        // A key whose value equals the default keeps its channel identity.
        ParamBlock k;
        k.setKey(ParamId::PositionX, 0, 0.0, Interp::Linear);
        k.setKey(ParamId::PositionX, 10, 20.0, Interp::Linear);
        INK_CHECK(k.isIdentityAt(0));   // exactly at the rest key
        INK_CHECK(!k.isIdentityAt(5));  // mid-move
        INK_CHECK(!k.isIdentityAt(10));
        INK_CHECK(k.isIdentityAt(-3));  // clamped before the first key = rest

        // Opacity counts too: keyed 1.0 (the default) is identity, keyed 0.5
        // is not, and a key with the WRONG default (ScaleX default is 1.0)
        // stores 1.0 as identity.
        ParamBlock o;
        o.setKey(ParamId::Opacity, 0, 1.0, Interp::Linear);
        INK_CHECK(o.isIdentityAt(0));
        o.setKey(ParamId::Opacity, 0, 0.5, Interp::Linear);
        INK_CHECK(!o.isIdentityAt(0));
        ParamBlock s;
        s.setKey(ParamId::ScaleX, 0, 1.0, Interp::Bezier);
        s.setKey(ParamId::ScaleY, 0, 1.0, Interp::Bezier);
        INK_CHECK(s.isIdentityAt(0));

        // Float slop from the 0..1 -> normalized-anchor round trip stays
        // inside the 1e-6 gate; a visible 0.01 step does not.
        ParamBlock a;
        a.setStaticValue(ParamId::AnchorX, 0.5 + 1e-9);
        INK_CHECK(a.isIdentityAt(0));
        a.setStaticValue(ParamId::AnchorX, 0.51);
        INK_CHECK(!a.isIdentityAt(0));
        std::printf("isIdentityAt OK\n");
    }

    // The reference FRAME: whoever authors the first key fixes the rect the
    // anchor and corners normalize against, so the tool's live handles and the
    // compositor's matrixFor are the same map. Legacy (frameless) blocks fall
    // back to whatever rect the caller passes — the canvas, in practice.
    {
        const Rect canvas{0, 0, 400, 300};
        const Rect ink{120, 90, 60, 40};

        ParamBlock legacy;
        INK_CHECK(!legacy.hasFrame());
        INK_CHECK(legacy.frameOr(canvas).x == canvas.x && legacy.frameOr(canvas).w == canvas.w);

        ParamBlock framed;
        framed.setFrame(ink);
        INK_CHECK(framed.hasFrame());
        INK_CHECK(framed.frameOr(canvas).x == 120 && framed.frameOr(canvas).w == 60);

        // A frame is metadata, NOT animation: it must not make a block
        // non-empty or non-identity (an untouched layer has to keep compositing
        // bit-identically, or every paint dab would pay a full re-projection).
        INK_CHECK(framed.empty());
        INK_CHECK(framed.isIdentityAt(0));
        framed.setFrame(Rect{}); // an empty rect is refused, never stored
        INK_CHECK(framed.hasFrame() && framed.frame().w == 60);

        // The frame is part of the block's identity for undo/redo compare.
        INK_CHECK(!(framed == legacy));
        ParamBlock copy = framed;
        INK_CHECK(copy == framed);
        copy.clearFrame();
        INK_CHECK(!(copy == framed));
        INK_CHECK(copy.empty()); // dropping the frame changes nothing else

        // A keyed anchor is read against the frame: 0.5 of the ink box is the
        // ink's own centre, not the page's.
        framed.setKey(ParamId::AnchorX, 0, 0.5, Interp::Linear);
        framed.setKey(ParamId::Rotation, 0, 90.0, Interp::Linear);
        const Rect src = framed.frameOr(canvas);
        const Mat3 m = framed.matrixFor(src, 0.0);
        const Vec2 centre{double(src.x + src.w / 2), double(src.y + src.h / 2)};
        const Vec2 turned = m.map(centre);
        INK_CHECK(near(turned.x, centre.x, 1e-6) && near(turned.y, centre.y, 1e-6));
        // ...and a corner follows the rotation about that centre.
        const Vec2 corner = m.map({double(src.x), double(src.y)});
        INK_CHECK(near(corner.x, centre.x + (src.h / 2), 1e-6) &&
                  near(corner.y, centre.y - (src.w / 2), 1e-6));
        std::printf("reference frame OK\n");
    }

    std::printf("test_params PASS\n");
    return 0;
}
