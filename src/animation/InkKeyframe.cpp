#include "animation/InkKeyframe.h"
#include <cmath>

namespace ink {

namespace {

// A handle can never collapse to zero reach: the segment must stay a function
// of time, and a zero-width handle makes the x-curve degenerate.
constexpr double kMinInfl = 0.01;

// How a segment interpolates. Hold is decided by the LEFT key alone (a hold
// runs forward from wherever it was set), and Bezier is claimed by either side,
// so setting one key to Bezier is enough to shape the segment leaving it.
Interp segmentMode(const ScalarKey &lo, const ScalarKey &hi) {
    if (lo.interp == Interp::Hold) return Interp::Hold;
    if (lo.interp == Interp::Bezier || hi.interp == Interp::Bezier) return Interp::Bezier;
    return Interp::Linear;
}

// The segment's control points in absolute (frame, value) space. x is
// normalized across the segment; y stays in channel units, which is what keeps
// a zero-delta segment well defined.
CubicSegment buildSegment(const ScalarKey &lo, const ScalarKey &hi) {
    CubicSegment s;
    s.f0 = lo.frame;
    s.f1 = hi.frame;
    s.v0 = lo.value;
    s.v1 = hi.value;
    s.kind = segmentMode(lo, hi);
    const double dt = double(s.f1 - s.f0);
    const double oi = std::clamp(double(lo.outInfl), kMinInfl, 1.0);
    const double ii = std::clamp(double(hi.inInfl), kMinInfl, 1.0);
    // The handle reaches oi*dt across the segment, so its normalized x is oi;
    // the incoming handle reaches ii*dt back from the far end.
    s.x = TimeCurve::make(oi, 1.0 - ii);
    s.p1y = s.v0 + double(lo.outSpeed) * oi * dt;
    s.p2y = s.v1 - double(hi.inSpeed) * ii * dt;
    return s;
}

double evaluateSegment(const CubicSegment &s, double frame) {
    const double dt = double(s.f1 - s.f0);
    if (dt <= 0.0) return s.v0;
    if (s.kind == Interp::Hold) return s.v0;
    const double t = (frame - double(s.f0)) / dt;
    if (s.kind == Interp::Linear) return s.v0 + t * (s.v1 - s.v0);
    const double u = bezierSolveT(t, s.x);
    return cubicValue(u, s.v0, s.p1y, s.p2y, s.v1);
}

} // namespace

void ScalarChannel::setKey(int f, float v, Interp i) {
    ScalarKey k = keys_.count(f) ? keys_[f] : ScalarKey{};
    k.frame = f;
    k.value = v;
    k.interp = i;
    keys_[f] = k;
}

bool ScalarChannel::removeKey(int f) { return keys_.erase(f) > 0; }
bool ScalarChannel::hasKey(int f) const { return keys_.count(f) > 0; }
const ScalarKey *ScalarChannel::keyAt(int f) const {
    auto it = keys_.find(f);
    return it == keys_.end() ? nullptr : &it->second;
}

std::vector<int> ScalarChannel::keys() const {
    std::vector<int> k;
    k.reserve(keys_.size());
    for (auto &[f, _] : keys_) k.push_back(f);
    return k;
}

int ScalarChannel::firstKey() const { return keys_.empty() ? -1 : keys_.begin()->first; }
int ScalarChannel::lastKey() const { return keys_.empty() ? -1 : keys_.rbegin()->first; }

int ScalarChannel::nextKey(int frame) const {
    auto it = keys_.upper_bound(frame);
    return it == keys_.end() ? -1 : it->first;
}

int ScalarChannel::prevKey(int frame) const {
    auto it = keys_.lower_bound(frame);
    if (it == keys_.begin()) return -1;
    return std::prev(it)->first;
}

float ScalarChannel::valueAt(int frame, float def) const {
    return float(valueAtF(double(frame), double(def)));
}

double ScalarChannel::valueAtF(double frame, double def) const {
    if (keys_.empty()) return def;
    const auto it = keys_.upper_bound((int)std::floor(frame));
    if (it == keys_.begin()) return it->second.value; // before the first key
    const auto lo = std::prev(it);
    // Landing exactly on a key returns the stored value verbatim: a key must
    // never drift off its own value through the solver.
    if (frame <= double(lo->first)) return lo->second.value;
    if (it == keys_.end()) return lo->second.value; // hold past the last key
    // Values are stored as float, so a key's own frame may evaluate to a float
    // neighbour that is 1e-6 away — within the solver that reads as a tiny
    // segment and a Bezier tangent can turn it into a visible wiggle. Within
    // epsilon of a key, the key's value IS the answer (Krita's time epsilon).
    if (std::abs(frame - double(it->first)) < 1e-6) return it->second.value;
    return evaluateSegment(buildSegment(lo->second, it->second), frame);
}

bool ScalarChannel::setInterp(int frame, Interp i) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    it->second.interp = i;
    return true;
}

bool ScalarChannel::setHandles(int frame, double outInfl, double outSpeed, double inInfl,
                               double inSpeed) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    it->second.outInfl = float(std::clamp(outInfl, kMinInfl, 1.0));
    it->second.outSpeed = float(outSpeed);
    it->second.inInfl = float(std::clamp(inInfl, kMinInfl, 1.0));
    it->second.inSpeed = float(inSpeed);
    return true;
}

bool ScalarChannel::setOutHandle(int frame, double infl, double speed) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    it->second.outInfl = float(std::clamp(infl, kMinInfl, 1.0));
    it->second.outSpeed = float(speed);
    return true;
}

bool ScalarChannel::setInHandle(int frame, double infl, double speed) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    it->second.inInfl = float(std::clamp(infl, kMinInfl, 1.0));
    it->second.inSpeed = float(speed);
    return true;
}

bool ScalarChannel::autoHandles(int frame) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    const double s = autoTangentSpeed(*this, frame);
    it->second.outSpeed = float(s);
    it->second.inSpeed = float(s);
    return true;
}

bool ScalarChannel::setAutoBez(int frame, bool on) {
    auto it = keys_.find(frame);
    if (it == keys_.end()) return false;
    it->second.autoBez = on;
    return true;
}

bool ScalarChannel::autoBez(int frame) const {
    const ScalarKey *k = keyAt(frame);
    return k && k->autoBez;
}

bool ScalarChannel::segment(int leftFrame, CubicSegment *out) const {
    const ScalarKey *lo = keyAt(leftFrame);
    if (!lo) return false;
    const int nxt = nextKey(leftFrame);
    if (nxt < 0) return false;
    const ScalarKey *hi = keyAt(nxt);
    if (!hi) return false;
    if (out) *out = buildSegment(*lo, *hi);
    return true;
}

Interp ScalarChannel::segmentInterp(int leftFrame) const {
    const ScalarKey *lo = keyAt(leftFrame);
    if (!lo) return Interp::Hold;
    const ScalarKey *hi = keyAt(nextKey(leftFrame));
    if (!hi) return lo->interp;
    return segmentMode(*lo, *hi);
}

void ScalarChannel::moveKey(int from, int to) {
    auto it = keys_.find(from);
    if (it == keys_.end()) return;
    ScalarKey k = it->second;
    keys_.erase(it);
    k.frame = to;
    keys_[to] = k;
}

int ScalarChannel::shiftFrom(int from, int delta, int maxFrame) {
    if (delta == 0) return 0;
    std::vector<ScalarKey> ks;
    ks.reserve(keys_.size());
    for (auto &[f, k] : keys_) ks.push_back(k);
    int moved = 0;
    if (delta > 0) {
        for (int i = (int)ks.size() - 1; i >= 0; --i) {
            if (ks[i].frame < from) break;
            keys_.erase(ks[i].frame);
            const int t = ks[i].frame + delta;
            if (t <= maxFrame) {
                ks[i].frame = t;
                keys_[t] = ks[i];
                ++moved;
            }
        }
    } else {
        for (auto &k : ks) {
            if (k.frame < from) continue;
            keys_.erase(k.frame);
            const int t = k.frame + delta;
            if (t >= from) {
                k.frame = t;
                keys_[t] = k;
                ++moved;
            }
        }
    }
    return moved;
}

int ScalarChannel::mirrorRange(int lo, int hi) {
    if (hi <= lo) return 0;
    std::vector<ScalarKey> in;
    for (auto &[f, k] : keys_)
        if (f >= lo && f <= hi) in.push_back(k);
    if (in.size() < 2) return 0;
    for (auto &k : in) keys_.erase(k.frame);
    for (auto &k : in) {
        k.frame = lo + (hi - k.frame);
        keys_[k.frame] = k;
    }
    return (int)in.size();
}

double autoTangentSpeed(const ScalarChannel &ch, int frame) {
    const ScalarKey *k = ch.keyAt(frame);
    if (!k) return 0.0;
    const int prev = ch.prevKey(frame);
    const int next = ch.nextKey(frame);
    if (prev >= 0 && next >= 0) {
        const ScalarKey *a = ch.keyAt(prev);
        const ScalarKey *b = ch.keyAt(next);
        const double df = double(next - prev);
        if (!a || !b || df <= 0.0) return 0.0;
        return (double(b->value) - double(a->value)) / df;
    }
    // One-sided at the ends of the channel: the single neighbour gives the chord.
    const int other = (next >= 0) ? next : prev;
    const ScalarKey *o = ch.keyAt(other);
    if (!o) return 0.0;
    const double df = double(other - frame);
    if (std::abs(df) < 1e-9) return 0.0;
    return (double(o->value) - double(k->value)) / df;
}

std::vector<int> RasterChannel::keys() const {
    std::vector<int> k; for (auto &[f,_] : keys_) k.push_back(f); return k;
}
int RasterChannel::activeAt(int frame) const {
    auto it = keys_.upper_bound(frame);
    if (it == keys_.begin()) return -1;
    return std::prev(it)->first;
}
int RasterChannel::nextKey(int frame) const {
    auto it = keys_.upper_bound(frame);
    return it == keys_.end() ? -1 : it->first;
}
int RasterChannel::prevKey(int frame) const {
    auto it = keys_.lower_bound(frame);
    if (it == keys_.begin()) return -1;
    return std::prev(it)->first;
}
int RasterChannel::rankOf(int key) const {
    auto it = keys_.find(key);
    if (it == keys_.end()) return -1;
    // std::map iterators are bidirectional: rank is a walk, but with no
    // allocation (keys() used to copy the whole vector per query).
    return (int)std::distance(keys_.begin(), it);
}

int RasterChannel::shiftFrom(int from, int delta, int maxFrame) {
    if (delta == 0) return 0;
    std::vector<int> ks = keys();
    int moved = 0;
    if (delta > 0) {
        for (int i = (int)ks.size() - 1; i >= 0; --i) {
            int k = ks[i];
            if (k < from) break;
            keys_.erase(k);
            int t = k + delta;
            if (t <= maxFrame) { keys_[t] = true; moved++; }
        }
    } else {
        for (int k : ks) {
            if (k < from) continue;
            keys_.erase(k);
            int t = k + delta;
            if (t >= from) { keys_[t] = true; moved++; }
            // keys pulled below `from` are dropped (hold collapse)
        }
    }
    return moved;
}

int RasterChannel::mirrorRange(int lo, int hi) {
    if (hi <= lo) return 0;
    std::vector<int> in;
    for (auto &[f, _] : keys_)
        if (f >= lo && f <= hi) in.push_back(f);
    if (in.size() < 2) return 0;
    for (int k : in) keys_.erase(k);
    for (int k : in) keys_[lo + (hi - k)] = true;
    return (int)in.size();
}

} // namespace ink
