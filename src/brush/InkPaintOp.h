#pragma once
// PaintOp framework: engine interface + registry + pixel brush + smudge.
// Production: stroke interpolator (spacing-based dab placement along path),
// dab cache keyed by quantized size/opacity, deterministic replay for redo.
#include "brush/InkBrushTip.h"
#include "brush/InkSensors.h"
#include "core/InkPaintDevice.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ink {

struct Dab {
    float x{0}, y{0};
    float sizeMul{1.0f};
    float opacityMul{1.0f};
};

struct StrokeEvent {
    TabletState state;
    double timeSec{0};
};

// Interpolates flattened path points into spaced dabs (handles fast strokes).
// Size/opacity multipliers lerp along each segment by arc-length fraction,
// so pressure transitions are smooth instead of stepping per event.
class StrokeInterpolator {
public:
    explicit StrokeInterpolator(float spacingPx) : spacingPx_(spacingPx) {}
    void reset(float x, float y, float sizeMul = 1.0f, float opacityMul = 1.0f);
    // Returns dabs to stamp for this point (0..N)
    std::vector<Dab> push(const TabletState &s, float sizeMul, float opacityMul);
    // Append-only variant: reuses the caller's vector (no per-event alloc).
    void pushTo(const TabletState &s, float sizeMul, float opacityMul, std::vector<Dab> &out);
private:
    float spacingPx_{3.0f};
    float lastX_{0}, lastY_{0};
    float lastSizeMul_{1.0f}, lastOpMul_{1.0f};
    float leftover_{0};
    bool started_{false};
};

struct PaintOpContext {
    PaintDevice *device{nullptr};
    PixelRGBA8 color{0,0,0,255};
    CompositeOp composite{CompositeOp::Normal};
    TileMemento *memento{nullptr}; // stroke-level undo capture
    Rect *dirtyAccum{nullptr};     // optional: union of dab rects (canvas
                                   // incremental projection consumes this)
    const Selection *selection{nullptr}; // optional selection clip
};

class PaintOp {
public:
    virtual ~PaintOp() = default;
    virtual std::string id() const = 0;
    virtual void beginStroke(PaintOpContext ctx) = 0;
    virtual void addEvent(const StrokeEvent &e) = 0;
    virtual void endStroke() = 0;
};

class PixelBrushOp : public PaintOp {
public:
    PixelBrushOp(AutoBrushTip tip, BrushDynamics dyn);
    std::string id() const override { return "pixel_brush"; }
    void beginStroke(PaintOpContext ctx) override;
    void addEvent(const StrokeEvent &e) override;
    void endStroke() override;
    void setTip(AutoBrushTip t);
private:
    // Event path: raw event -> sensor curves -> stabilizer -> Bezier curve
    // fitter -> arc-length spacing walker -> resampled dab stamp.
    // (Krita pipeline concept: smoothing in the freehand stage, dab density
    // in the paint stage.)
    void stampDab(const Dab &d);
    AutoBrushTip tip_;
    BrushDynamics dyn_;
    PaintOpContext ctx_;
    StrokeInterpolator interp_{3.0f};
    Stabilizer stab_;
    CurveFlattener flat_;
    bool stroking_{false};
    // Per-event scratch (reused, never reallocated in steady state) plus a
    // cache of pressure-resampled tip masks keyed by odd dab size: the old
    // path re-ran an O(ns^2) bilinear resample AND a heap alloc per dab
    // whenever pressure moved off the cached size.
    std::vector<Dab> dabScratch_;
    std::vector<PathPoint> pathScratch_;
    std::unordered_map<int, std::vector<float>> maskCache_;
};

class EraserOp : public PixelBrushOp {
public:
    using PixelBrushOp::PixelBrushOp;
    std::string id() const override { return "eraser"; }
};

class PaintOpRegistry {
public:
    using Factory = std::function<std::unique_ptr<PaintOp>()>;
    static PaintOpRegistry &instance();
    void reg(const std::string &id, Factory f);
    std::unique_ptr<PaintOp> create(const std::string &id) const;
    std::vector<std::string> ids() const;
private:
    std::unordered_map<std::string, Factory> map_;
};

} // namespace ink
