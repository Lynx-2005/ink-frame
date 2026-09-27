#pragma once
// Keyframe channels: raster holds + scalar interpolation.
// Production: per-layer channel set, hold/linear/bezier modes for scalars,
// blank-vs-hold distinction, add/remove/move with undo-friendly structs.
// A raster key owns an explicit exposure SPAN [key, spanEnd]; the span is
// stored per layer (PaintLayer), never derived from the next key, so resizing
// one cell can't resize another.
//
// Scalar interpolation is a cubic Bezier over time. Keys sit on integer frames
// (the raster model's unit); the HANDLES are continuous, which is where smooth
// motion actually comes from. A handle is stored the way animators think about
// it — influence and speed — not as an opaque control point:
//   influence  how far the handle reaches across its segment, as a fraction of
//              the segment duration (0.01 .. 1). 1/3 is the neutral value.
//   speed      value units per frame along the handle (the UI multiplies by
//              fps for the familiar units-per-second readout).
// Absolute speeds rather than fractions of the value delta: a segment whose two
// keys hold the same value has no delta, and normalizing by it would make flat
// segments — and the handles on them — degenerate.
#include "animation/InkEasing.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <vector>

namespace ink {

struct TimeSpan { int start{0}, end{0}; }; // inclusive
inline int clampFrame(int f, int lo, int hi) { return std::clamp(f, lo, hi); }

enum class Interp { Hold, Linear, Bezier };

struct ScalarKey {
    int frame{0};
    float value{0};
    Interp interp{Interp::Hold};
    // Outgoing handle (this key -> next key) and incoming handle (previous key
    // -> this key). Only consulted for segments that actually interpolate as
    // Bezier; Hold and Linear ignore them without losing them, so switching a
    // key's mode back and forth is lossless.
    float outInfl{0.333333f}, outSpeed{0.0f};
    float inInfl{0.333333f}, inSpeed{0.0f};
    // True while this key's handles are DERIVED from its neighbours rather than
    // hand-set. Auto keys are recomputed whenever the channel's shape changes,
    // which is what keeps a recorded drag a straight line instead of leaving
    // each key frozen with the tangent its neighbours happened to imply at the
    // moment it was created. Dragging a handle clears this flag (see After
    // Effects' auto vs. free bezier distinction).
    bool autoBez{true};
};

// One evaluated segment, in absolute (frame, value) space — exactly the curve
// the compositor interpolates. The graph editor draws these control points, so
// the drawn curve and the evaluated curve can never disagree.
struct CubicSegment {
    int f0{0}, f1{0};
    double v0{0}, v1{0};
    TimeCurve x{};
    double p1y{0}, p2y{0};
    Interp kind{Interp::Linear};
};

class ScalarChannel {
public:
    void setKey(int frame, float value, Interp interp = Interp::Hold);
    bool removeKey(int frame);
    void clear() { keys_.clear(); }
    bool hasKey(int frame) const;
    const ScalarKey *keyAt(int frame) const;
    std::vector<int> keys() const;
    int keyCount() const { return (int)keys_.size(); }
    bool empty() const { return keys_.empty(); }
    int firstKey() const;
    int lastKey() const;
    int prevKey(int frame) const;
    int nextKey(int frame) const;

    float valueAt(int frame, float def = 1.0f) const;
    // Fractional-frame evaluation: the compositor walks whole frames, but the
    // graph editor and any future sub-frame sampler read between them.
    double valueAtF(double frame, double def = 1.0) const;

    bool setInterp(int frame, Interp i);
    bool setHandles(int frame, double outInfl, double outSpeed, double inInfl, double inSpeed);
    bool setOutHandle(int frame, double infl, double speed);
    bool setInHandle(int frame, double infl, double speed);
    // Replace a key's handles with the auto (neighbour-derived) tangent.
    bool autoHandles(int frame);
    // Auto (true) or free (false) handles. Free means the user shaped them.
    bool setAutoBez(int frame, bool on);
    bool autoBez(int frame) const;

    // How this channel interpolates out of `leftFrame`: the segment's mode and
    // its exact control points. False when there is no segment starting there.
    bool segment(int leftFrame, CubicSegment *out) const;
    // Reconciled mode of the segment leaving `leftFrame` (Hold when either side
    // holds, Bezier when either side asks for it, else Linear).
    Interp segmentInterp(int leftFrame) const;

    void moveKey(int from, int to);
    // Shift every key >= from by delta (clamped to [0, maxFrame]). Returns keys
    // moved or dropped.
    int shiftFrom(int from, int delta, int maxFrame);
    // Reflect key positions within [lo, hi]. Returns keys moved.
    int mirrorRange(int lo, int hi);

private:
    std::map<int, ScalarKey> keys_;
};

// Auto (neighbour-derived) tangent speed for a key, in value units per frame:
// the chord slope through the key's neighbours. Chosen because it reduces a
// collinear run of keys to an exact straight line — a freshly keyed channel
// never wobbles on its own. One-sided at the first and last key.
double autoTangentSpeed(const ScalarChannel &ch, int frame);

// Raster channel = ordered key set (holds). No interpolation of pixels.
class RasterChannel {
public:
    void setKey(int frame) { keys_[frame] = true; }
    bool removeKey(int frame) { return keys_.erase(frame) > 0; }
    bool hasKey(int frame) const { return keys_.count(frame) > 0; }
    int keyCount() const { return (int)keys_.size(); }
    bool empty() const { return keys_.empty(); }
    std::vector<int> keys() const;
    // Active (hold) key <= frame, or -1 if none
    int activeAt(int frame) const;
    int nextKey(int frame) const;
    int prevKey(int frame) const;
    // Ordinal of `key` in sorted order, or -1 when absent. Allocation-free
    // (the old cellIndexAt copied the whole key vector per query).
    int rankOf(int key) const;
    // Shift all keys >= from by delta (clamped to [0, maxFrame]).
    // Positive delta processes high->low so holds extend correctly;
    // keys pushed past maxFrame are dropped. Returns keys moved.
    int shiftFrom(int from, int delta, int maxFrame);
    // Reflect key positions within [lo, hi]. Returns keys moved.
    int mirrorRange(int lo, int hi);
private:
    std::map<int, bool> keys_;
};

} // namespace ink
