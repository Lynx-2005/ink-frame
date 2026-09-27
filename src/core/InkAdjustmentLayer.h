#pragma once
// Compatibility facade over the keyframes that now live ON the paint layer.
//
// The adjustment layer itself is gone from the app: the timeline, the tools and
// the compositor all animate paint layers directly (InkLayer.h). This type
// remains only so the existing call sites — the curve editor, the transform
// panel and the tests — keep compiling against a stable name while they are
// migrated to `PaintLayer*`. It is a non-owning VIEW: `params()` returns the
// target layer's own ParamBlock, so every read and write lands on the real
// keyframes. It is never inserted into the layer tree and renders nothing.
#include "core/InkLayer.h"
#include "core/InkParams.h"
#include <algorithm>

namespace ink {

class AdjustmentLayer : public Layer {
public:
    explicit AdjustmentLayer(std::string name)
        : Layer(std::move(name), LayerType::Adjustment) {}

    // The view IS its target: every id-based lookup and every title shown in
    // the UI must report the paint layer, not this facade's own Layer identity.
    std::string stableId() const {
        return targetLayer_ ? targetLayer_->stableId() : Layer::stableId();
    }
    const std::string &name() const { return targetLayer_ ? targetLayer_->name() : Layer::name(); }

    // --- the view target -------------------------------------------------
    // The paint layer whose keyframes this facade exposes. 0 = unlinked: the
    // layer evaluates to the identity and composites nothing.
    uint64_t targetId() const { return target_; }
    void setTargetId(uint64_t id) { target_ = id; }
    PaintLayer *target() const { return targetLayer_; }
    void setTargetLayer(PaintLayer *l) {
        targetLayer_ = l;
        target_ = l ? l->id() : 0;
    }
    bool linked() const { return targetLayer_ != nullptr; }

    // --- active span (kept for the timeline's bar geometry) --------------
    // The whole project: keyframes on the layer are valid at every frame, so
    // the bar always reports the full span.
    int barStart() const { return 0; }
    int barEnd() const { return -1; }
    bool barOpen() const { return true; }
    bool activeAt(double) const { return true; }
    int barEndFor(int lastFrame) const { return lastFrame; }
    int barLength(int lastFrame) const { return lastFrame + 1; }
    void clampBarTo(int) {}

    // --- params: the TARGET's, by reference ------------------------------
    ParamBlock &params() {
        static ParamBlock empty;
        return targetLayer_ ? targetLayer_->params() : empty;
    }
    const ParamBlock &params() const {
        static ParamBlock empty;
        return targetLayer_ ? targetLayer_->params() : empty;
    }
    bool animated() const { return targetLayer_ && targetLayer_->animated(); }

    // Source rect = the target's content bounds at that frame; the anchor and
    // corner parameters are normalized against it.
    Mat3 matrixFor(const Rect &src, double frame, float *opacityOut = nullptr) const {
        return params().matrixFor(src, frame, opacityOut);
    }

private:
    uint64_t target_{0};
    PaintLayer *targetLayer_{nullptr};
};

} // namespace ink
