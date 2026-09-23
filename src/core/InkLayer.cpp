#include "core/InkLayer.h"
#include <algorithm>

namespace ink {

uint64_t Layer::s_nextId_{1};

Layer::Layer(std::string name, LayerType type) : id_(s_nextId_++), name_(std::move(name)), type_(type) {}

void Layer::setOpacity(float o) { opacity_ = std::clamp(o, 0.0f, 1.0f); }

PaintDevice *PaintLayer::deviceAt(int frame, bool create) {
    auto it = frames_.find(frame);
    if (it != frames_.end()) return it->second.get();
    if (!create) return nullptr;
    auto d = std::make_unique<PaintDevice>();
    auto *p = d.get();
    frames_[frame] = std::move(d);
    return p;
}
const PaintDevice *PaintLayer::deviceAt(int frame) const {
    auto it = frames_.find(frame);
    return it == frames_.end() ? nullptr : it->second.get();
}
bool PaintLayer::hasKeyAt(int frame) const { return frames_.count(frame) > 0; }
void PaintLayer::setKeyAt(int frame, bool hasKey) {
    if (hasKey) {
        if (hasKeyAt(frame)) return;
        // A new key splits any longer cell before it, and starts out holding
        // to the next key / the end of the project (callers that want a
        // one-frame cell set the span explicitly).
        auto next = frames_.lower_bound(frame);
        if (next != frames_.begin()) {
            auto prev = std::prev(next);
            if (spanEnd(prev->first) >= frame) spanEnd_[prev->first] = frame - 1;
        }
        frames_[frame] = std::make_unique<PaintDevice>();
        spanEnd_[frame] = kOpenEnd;
    } else {
        frames_.erase(frame);
        spanEnd_.erase(frame);
    }
}
std::vector<int> PaintLayer::keys() const {
    std::vector<int> k; k.reserve(frames_.size());
    for (auto &[f,_] : frames_) k.push_back(f);
    return k;
}

int PaintLayer::spanEnd(int key) const {
    auto it = spanEnd_.find(key);
    if (it != spanEnd_.end()) return it->second;
    if (!hasKeyAt(key)) return key;
    auto next = frames_.upper_bound(key); // legacy keys: hold to the next one
    return next == frames_.end() ? kOpenEnd : next->first - 1;
}

int PaintLayer::spanEndClamped(int key, int maxFrame) const {
    int e = spanEnd(key);
    if (e == kOpenEnd) e = maxFrame;
    return std::clamp(e, key, std::max(key, maxFrame));
}

void PaintLayer::setSpanEnd(int key, int end) {
    if (!hasKeyAt(key)) return;
    if (end < key) end = key; // a cell is at least one frame long
    auto next = frames_.upper_bound(key);
    if (next != frames_.end() && end >= next->first) end = next->first - 1;
    spanEnd_[key] = end;
}

int PaintLayer::exposureAt(int frame) const {
    auto it = frames_.upper_bound(frame);
    if (it == frames_.begin()) return -1;
    --it;
    const int key = it->first;
    const int e = spanEnd(key);
    return (e == kOpenEnd || e >= frame) ? key : -1;
}

void PaintLayer::copyDeviceContent(int fromKey, int toKey) {
    if (fromKey == toKey) return;
    auto it = frames_.find(fromKey);
    if (it == frames_.end() || !it->second) return;
    const int end = spanEnd(fromKey);
    PaintDevice *dst = deviceAt(toKey, true);
    if (!dst) return;
    dst->clear();
    if (!it->second->extent().isEmpty())
        dst->blit(*it->second, 0, 0);
    spanEnd_[toKey] = (end == kOpenEnd) ? kOpenEnd : toKey + (end - fromKey);
    setSpanEnd(toKey, spanEnd_[toKey]); // keep the copy inside the next key
}

bool PaintLayer::moveDeviceContent(int fromKey, int toKey, bool overwrite) {
    if (fromKey == toKey) return true;
    auto it = frames_.find(fromKey);
    if (it == frames_.end()) return false;
    if (hasKeyAt(toKey) && !overwrite) return false;
    const int end = spanEnd(fromKey);
    std::unique_ptr<PaintDevice> dev = std::move(it->second);
    frames_.erase(it);
    spanEnd_.erase(fromKey);
    frames_[toKey] = std::move(dev);
    // the exposure travels with the drawing
    spanEnd_[toKey] = (end == kOpenEnd) ? kOpenEnd : toKey + (end - fromKey);
    setSpanEnd(toKey, spanEnd_[toKey]);
    return true;
}

int PaintLayer::shiftFrom(int from, int delta, int maxFrame) {
    if (delta == 0) return 0;
    int moved = 0;
    if (delta > 0) {
        std::vector<int> ks = keys();
        for (auto it = ks.rbegin(); it != ks.rend(); ++it) { // high -> low
            const int k = *it;
            if (k < from) break;
            const int t = k + delta;
            if (t > maxFrame) { setKeyAt(k, false); ++moved; continue; }
            if (hasKeyAt(t)) continue; // never clobber
            if (moveDeviceContent(k, t, false)) ++moved;
        }
    } else {
        for (int k : keys()) {
            if (k < from) continue;
            const int t = k + delta;
            if (t < 0 || hasKeyAt(t)) continue;
            if (moveDeviceContent(k, t, false)) ++moved;
        }
    }
    return moved;
}

void GroupLayer::removeChild(size_t i) {
    if (i < children_.size()) children_.erase(children_.begin()+i);
}

} // namespace ink
