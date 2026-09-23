#include "animation/InkKeyframe.h"

namespace ink {

void ScalarChannel::setKey(int f, float v, Interp i) { keys_[f] = {f, v, i}; }
bool ScalarChannel::removeKey(int f) { return keys_.erase(f) > 0; }
bool ScalarChannel::hasKey(int f) const { return keys_.count(f) > 0; }
std::vector<int> ScalarChannel::keys() const {
    std::vector<int> k; for (auto &[f,_] : keys_) k.push_back(f); return k;
}
float ScalarChannel::valueAt(int frame, float def) const {
    if (keys_.empty()) return def;
    auto it = keys_.upper_bound(frame);
    if (it == keys_.begin()) return it->second.value;
    auto lo = std::prev(it == keys_.end() ? it : it);
    // upper bound semantics: lo <= frame < hi
    if (it == keys_.end()) {
        return lo->second.value; // hold past last
    }
    if (lo->second.interp == Interp::Hold) return lo->second.value;
    // linear
    int f0 = lo->first, f1 = it->first;
    if (f1 == f0) return lo->second.value;
    float t = float(frame - f0) / float(f1 - f0);
    return lo->second.value + t * (it->second.value - lo->second.value);
}
void ScalarChannel::moveKey(int from, int to) {
    auto it = keys_.find(from);
    if (it == keys_.end()) return;
    ScalarKey k = it->second; keys_.erase(it);
    k.frame = to; keys_[to] = k;
}

std::vector<int> RasterChannel::keys() const {
    std::vector<int> k; for (auto &[f,_] : keys_) k.push_back(f); return k;
}
int RasterChannel::activeAt(int frame) const {
    int best = -1;
    for (auto &[f,_] : keys_) { if (f <= frame) best = f; else break; }
    return best;
}
int RasterChannel::nextKey(int frame) const {
    for (auto &[f,_] : keys_) if (f > frame) return f;
    return -1;
}
int RasterChannel::prevKey(int frame) const {
    int best = -1;
    for (auto &[f,_] : keys_) { if (f < frame) best = f; else break; }
    return best;
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
