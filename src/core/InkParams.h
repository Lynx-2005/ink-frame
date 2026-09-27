#pragma once
// Animatable parameter set: the thing an adjustment layer actually keyframes.
//
// A ParamBlock is a named bundle of scalar channels plus a static (un-keyed)
// value per parameter, exactly the After Effects model: a property with the
// stopwatch off has one value and no influence on the timeline; switching the
// stopwatch on turns that value into the first key. Reading a parameter always
// goes through valueAt(), so callers never care which of the two states a
// parameter is in.
//
// The parameter set is deliberately data, not code: ParamId + paramTable() are
// the single source of truth for names, defaults, ranges and units, so adding
// an animatable parameter to a tool means adding a row, not touching the
// compositor, the serializer, or the timeline.
//
// Geometry convention (matches FreeTransformParams in InkTransform.h so the
// tool and the channel agree exactly, once both are handed the same rect):
//   frame    THE reference rect the anchor and corners normalize against.
//            Whoever authors the first key fixes it — normally the transform
//            tool from the lifted content bbox (selection, else content
//            extent) — and it never moves afterwards, so the pivot cannot
//            drift when the artist draws one more pixel. A block with no
//            frame is legacy: every call site falls back to the canvas rect,
//            which is what canvas-normalized keys always meant.
//   anchor   NORMALIZED to the frame (0.5, 0.5 = its center).
//   position document pixels added on top of the identity placement.
//   matrix   T(position) . T(anchor) . R . Shear . Scale . T(-anchor)
//   corners  NORMALIZED to the frame, canonical order TL,TR,BR,BL; they act
//            FIRST in source space, like the tool's projective override.
// The empty block resolves to the identity matrix and opacity 1, so a layer
// carrying an un-keyed adjustment layer composites bit-identically to one
// carrying none.
#include "animation/InkKeyframe.h"
#include "core/InkTransform.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace ink {

enum class ParamId : int {
    AnchorX = 0, AnchorY,
    PositionX, PositionY,
    ScaleX, ScaleY,
    Rotation,
    ShearX, ShearY,
    Opacity,
    Corner0X, Corner0Y,
    Corner1X, Corner1Y,
    Corner2X, Corner2Y,
    Corner3X, Corner3Y,
    Count
};
inline constexpr int kParamCount = (int)ParamId::Count;

enum class ParamGroup : int { Transform = 0, Perspective = 1 };

struct ParamDesc {
    ParamId id;
    const char *key;    // stable token for project.json
    const char *label;  // UI label
    const char *suffix; // "" | "px" | "%" | "deg"
    double def;         // value with no key and no static override
    double lo, hi;      // sane editing range (clamping, not a hard limit)
    bool angle;
    ParamGroup group;
};

// kParamCount entries indexed by ParamId. Single source of truth.
const ParamDesc *paramTable();
inline const ParamDesc &paramDesc(ParamId id) { return paramTable()[(int)id]; }
const ParamDesc *paramFromKey(const std::string &key); // nullptr when unknown

// Parameters the "Add keyframe" button keys when nothing is animated yet: the
// whole transform — position, pivot, scale, rotation, shear, opacity AND the
// perspective corners. A partial set froze the unkeyed channels between two
// keys (a lone key holds backwards), so this must stay complete.
const std::vector<ParamId> &defaultKeySet();

class ParamBlock {
public:
    // --- keys ---------------------------------------------------------------
    // Insert (or update) a key. A NEW Bezier key gets its handles derived from
    // its neighbours, so keying twice never makes motion wobble on its own.
    bool setKey(ParamId id, int frame, double value, Interp interp = Interp::Bezier,
                bool updateExisting = true, bool deriveHandles = true);
    bool removeKey(ParamId id, int frame);
    int removeKeysAt(int frame); // every parameter keyed at that frame
    bool hasKey(ParamId id, int frame) const;
    bool hasAnyKeyAt(int frame) const;
    std::vector<ParamId> keysAt(int frame) const;

    bool moveKey(ParamId id, int from, int to);
    bool setKeyValue(ParamId id, int frame, double value);
    bool setInterp(ParamId id, int frame, Interp i);
    bool setHandles(ParamId id, int frame, double outInfl, double outSpeed, double inInfl,
                    double inSpeed);
    bool autoHandles(ParamId id, int frame);
    // Recompute every AUTO key's tangent from its neighbours. Called whenever a
    // channel's shape changes, so a run of recorded keys stays a straight line
    // instead of keeping the tangent each key was born with.
    void refreshAutoHandles(ParamId id);
    void refreshAllAutoHandles();

    // --- queries ------------------------------------------------------------
    const ScalarChannel *channel(ParamId id) const;    bool isAnimated(ParamId id) const;
    std::vector<ParamId> animatedParams() const; // table order, animated only
    std::vector<int> keys(ParamId id) const;
    int keyCount() const;
    int prevKey(ParamId id, int frame) const;
    int nextKey(ParamId id, int frame) const;
    int firstKey(ParamId id) const;
    int lastKey(ParamId id) const;

    // --- values -------------------------------------------------------------
    // Keyed parameter -> the channel's value at that time; un-keyed -> its
    // static value. Fractional frames are fine.
    double valueAt(ParamId id, double frame) const;
    double staticValue(ParamId id) const;
    void setStaticValue(ParamId id, double v);
    void clearParam(ParamId id);

    bool empty() const; // no keys and every static value at its default
    // True when EVERY parameter EVALUATES to its default at `frame` (tolerance
    // 1e-6): a block that is not `empty()` — it has keys — but whose curve sits
    // at its rest value right here. The compositor uses this to treat such a
    // frame as if no adjustment existed: no lift/resample/stamp, and crucially
    // no full-canvas fallback in projectRect, which is what keeps painting
    // incremental whenever the animation happens to pass through rest.
    // This is deliberately per-frame, not per-block: opacity counted — a
    // keyed opacity of 1.0 at this frame is also identity.
    bool isIdentityAt(double frame) const;

    // --- resolution ---------------------------------------------------------
    // The composite transform for a source rect at a time. Identity (and
    // opacity 1) when the block is empty.
    Mat3 matrixFor(const Rect &sourceRect, double frame, float *opacityOut = nullptr) const;
    // The reference rect the anchor/corners normalize against: the stored
    // frame, or `fallback` for a legacy (frameless) block. Every projection
    // call site resolves through this, never by assuming the canvas.
    Rect frameOr(const Rect &fallback) const {
        return (hasFrame_ && !frame_.isEmpty()) ? frame_ : fallback;
    }
    bool hasFrame() const { return hasFrame_ && !frame_.isEmpty(); }
    Rect frame() const { return frame_; }
    // Set-once metadata, not animation: only call on a block with no animated
    // parameters (else the stored values would be re-read against a new
    // frame). Never affects empty()/isIdentityAt().
    void setFrame(const Rect &r) {
        if (!r.isEmpty()) {
            frame_ = r;
            hasFrame_ = true;
        }
    }
    void clearFrame() {
        frame_ = Rect{};
        hasFrame_ = false;
    }

    void shiftKeys(int from, int delta, int maxFrame);
    int mirrorRange(int lo, int hi);
    bool operator==(const ParamBlock &o) const;
    bool operator!=(const ParamBlock &o) const { return !(*this == o); }

private:
    // Recompute the auto tangent of ONE key when it is due (auto + Bezier):
    // the shared primitive behind the localized refresh below. Skips free
    // handles and non-Bezier keys exactly like refreshAutoHandles does.
    void refreshOneKey(ParamId id, int frame);
    // Recompute auto tangents for `frame` and its immediate neighbours. An
    // auto speed is the chord through the key's neighbours, so a single-key
    // edit (insert/remove/move/value/interp) can only change the tangents of
    // the edited key and the two keys beside it. Exactly equivalent to
    // refreshAutoHandles(id) for such edits while staying O(1) instead of
    // O(K) -- bulk keying drops from O(K^2) to O(K log K).
    void refreshAutoHandlesNear(ParamId id, int frame);
    // Array-indexed by ParamId (not std::map): the compositor evaluates every
    // parameter per animated layer per frame, and the tree walk per entry
    // dominated that path. An un-keyed channel is an empty shell — every
    // query treats empty as absent, exactly like a missing map entry did.
    static int idx(ParamId id) { return (int)id; }
    std::array<ScalarChannel, kParamCount> ch_{};
    std::array<double, kParamCount> stat_{};
    std::array<bool, kParamCount> hasStat_{};
    Rect frame_{};
    bool hasFrame_{false};
};

} // namespace ink
