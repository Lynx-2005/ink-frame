#pragma once
// Keyframe channels: raster holds + scalar (opacity) interpolation.
// Production: per-layer channel set, hold/linear/bezier modes for scalars,
// blank-vs-hold distinction, add/remove/move with undo-friendly structs.
// A raster key owns an explicit exposure SPAN [key, spanEnd]; the span is
// stored per layer (PaintLayer), never derived from the next key, so resizing
// one cell can't resize another.
#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace ink {

struct TimeSpan { int start{0}, end{0}; }; // inclusive
inline int clampFrame(int f, int lo, int hi) { return std::clamp(f, lo, hi); }

enum class Interp { Hold, Linear };

struct ScalarKey { int frame{0}; float value{0}; Interp interp{Interp::Hold}; };

class ScalarChannel {
public:
    void setKey(int frame, float value, Interp interp = Interp::Hold);
    bool removeKey(int frame);
    bool hasKey(int frame) const;
    std::vector<int> keys() const;
    float valueAt(int frame, float def = 1.0f) const;
    void moveKey(int from, int to);
private:
    std::map<int, ScalarKey> keys_;
};

// Raster channel = ordered key set (holds). No interpolation of pixels.
class RasterChannel {
public:
    void setKey(int frame) { keys_[frame] = true; }
    bool removeKey(int frame) { return keys_.erase(frame) > 0; }
    bool hasKey(int frame) const { return keys_.count(frame) > 0; }
    std::vector<int> keys() const;
    // Active (hold) key <= frame, or -1 if none
    int activeAt(int frame) const;
    int nextKey(int frame) const;
    int prevKey(int frame) const;
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
