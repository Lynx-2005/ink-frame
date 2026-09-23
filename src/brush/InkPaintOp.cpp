#include "brush/InkPaintOp.h"
#include <cmath>

namespace ink {

void StrokeInterpolator::reset(float x, float y, float sizeMul, float opacityMul) {
    lastX_ = x;
    lastY_ = y;
    lastSizeMul_ = sizeMul;
    lastOpMul_ = opacityMul;
    leftover_ = 0;
    started_ = true;
}

std::vector<Dab> StrokeInterpolator::push(const TabletState &s, float sizeMul, float opacityMul) {
    std::vector<Dab> out;
    if (!started_) {
        reset(s.x, s.y, sizeMul, opacityMul);
        out.push_back({s.x, s.y, sizeMul, opacityMul});
        return out;
    }
    float dx = s.x - lastX_, dy = s.y - lastY_;
    float dist = std::sqrt(dx * dx + dy * dy);
    float spacing = std::max(1.0f, spacingPx_);
    if (dist < 1e-6f) return out;
    float nx = dx / dist, ny = dy / dist;
    // Emit at spacing boundaries measured from the last dab (leftover_ is
    // the distance already travelled toward the next one). Multipliers lerp
    // by arc-length fraction so pressure transitions stay smooth.
    float d = spacing - leftover_;
    while (d <= dist) {
        float f = d / dist;
        out.push_back({lastX_ + nx * d, lastY_ + ny * d,
                       lastSizeMul_ + (sizeMul - lastSizeMul_) * f,
                       lastOpMul_ + (opacityMul - lastOpMul_) * f});
        d += spacing;
    }
    leftover_ = dist - (d - spacing); // travelled remainder in [0, spacing]
    lastX_ = s.x;
    lastY_ = s.y;
    lastSizeMul_ = sizeMul;
    lastOpMul_ = opacityMul;
    return out;
}

PixelBrushOp::PixelBrushOp(AutoBrushTip tip, BrushDynamics dyn)
    : tip_(std::move(tip)), dyn_(dyn), interp_(tip_.spacingPx()), stab_(dyn_.smoothing) {}

void PixelBrushOp::setTip(AutoBrushTip t) {
    tip_ = std::move(t);
    interp_ = StrokeInterpolator(tip_.spacingPx());
}

void PixelBrushOp::beginStroke(PaintOpContext ctx) {
    ctx_ = ctx;
    if (id() == "eraser") ctx_.composite = CompositeOp::Erase;
    stab_.setConfig(dyn_.smoothing);
    stroking_ = false;
}

// Bilinear resample of the tip mask to an odd-sized square (pressure size).
static std::vector<float> resampleMask(const std::vector<float> &src, int srcSize, int dstSize) {
    std::vector<float> dst(dstSize * dstSize, 0.0f);
    if (srcSize <= 1 || dstSize <= 1) return dst;
    double scale = double(srcSize - 1) / double(dstSize - 1);
    for (int y = 0; y < dstSize; ++y) {
        double sy = y * scale;
        int y0 = int(sy);
        int y1 = std::min(y0 + 1, srcSize - 1);
        double fy = sy - y0;
        for (int x = 0; x < dstSize; ++x) {
            double sx = x * scale;
            int x0 = int(sx);
            int x1 = std::min(x0 + 1, srcSize - 1);
            double fx = sx - x0;
            float v = float(src[y0 * srcSize + x0] * (1 - fx) * (1 - fy) +
                            src[y0 * srcSize + x1] * fx * (1 - fy) +
                            src[y1 * srcSize + x0] * (1 - fx) * fy +
                            src[y1 * srcSize + x1] * fx * fy);
            dst[y * dstSize + x] = v;
        }
    }
    return dst;
}

void PixelBrushOp::stampDab(const Dab &d) {
    if (!ctx_.device) return;
    const auto &base = tip_.mask();
    const int bs = tip_.maskSize();
    // Effective diameter follows pressure; resample mask when it moved off
    // the cached size so light/heavy touches actually change dab size.
    int ns = int(std::round(tip_.shape().diameter * std::clamp(d.sizeMul, 0.05f, 3.0f)));
    ns = std::clamp(ns | 1, 3, 513); // keep odd
    float effOpacity = tip_.shape().opacity * d.opacityMul;
    if (effOpacity <= 0.003f) return;
    Rect dabRect;
    if (ns == bs) {
        dabRect = ctx_.device->applyDab(d.x, d.y, base.data(), bs, ctx_.color, effOpacity,
                                        ctx_.composite, ctx_.memento, ctx_.selection);
    } else {
        auto m = resampleMask(base, bs, ns);
        dabRect = ctx_.device->applyDab(d.x, d.y, m.data(), ns, ctx_.color, effOpacity,
                                        ctx_.composite, ctx_.memento, ctx_.selection);
    }
    if (ctx_.dirtyAccum) *ctx_.dirtyAccum = uniteRect(*ctx_.dirtyAccum, dabRect);
}

void PixelBrushOp::addEvent(const StrokeEvent &e) {
    if (!ctx_.device) return;
    const float sm = dyn_.sizeMultiplier(e.state);
    const float om = dyn_.opacityMultiplier(e.state);
    const RawSample raw{double(e.state.x), double(e.state.y), e.state.pressure, e.timeSec};
    TabletState ps; // position carrier for the spacing walker
    if (!stroking_) {
        stab_.reset(raw);
        const RawSample s0 = stab_.push(raw);
        const PathPoint p0{s0.x, s0.y, sm, om};
        flat_.reset(p0);
        interp_.reset(float(p0.x), float(p0.y), sm, om);
        stroking_ = true;
        stampDab({float(p0.x), float(p0.y), sm, om});
        return;
    }
    const RawSample s = stab_.push(raw);
    for (const PathPoint &pt : flat_.push({s.x, s.y, sm, om})) {
        ps.x = float(pt.x);
        ps.y = float(pt.y);
        for (const Dab &d : interp_.push(ps, pt.sizeMul, pt.opacityMul)) stampDab(d);
    }
}

void PixelBrushOp::endStroke() {
    if (!stroking_) return;
    if (ctx_.device) {
        TabletState ps;
        for (const PathPoint &pt : flat_.finish()) {
            ps.x = float(pt.x);
            ps.y = float(pt.y);
            for (const Dab &d : interp_.push(ps, pt.sizeMul, pt.opacityMul)) stampDab(d);
        }
    }
    stroking_ = false;
}

PaintOpRegistry &PaintOpRegistry::instance() {
    static PaintOpRegistry r;
    return r;
}
void PaintOpRegistry::reg(const std::string &id, Factory f) { map_[id] = std::move(f); }
std::unique_ptr<PaintOp> PaintOpRegistry::create(const std::string &id) const {
    auto it = map_.find(id);
    return it == map_.end() ? nullptr : it->second();
}
std::vector<std::string> PaintOpRegistry::ids() const {
    std::vector<std::string> v;
    for (auto &[k, _] : map_) v.push_back(k);
    return v;
}

} // namespace ink
