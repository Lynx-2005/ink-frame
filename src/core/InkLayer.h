#pragma once
// Layer stack + keyframed raster frames. Production-grade:
// - paint/group layers, opacity, visibility, blend, lock
// - each paint layer owns map<frame, Cell> (sparse holds); a Cell is that
//   drawing's OWN layer stack (InkCell.h) — unique per cell
// - active frame resolution (hold semantics in animation layer)
// - projection = recursive composite for a given frame
#include "core/InkCell.h"
#include "core/InkLayerTypes.h"
#include "core/InkPaintDevice.h"
#include "core/InkParams.h"
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ink {

// Sentinel for an exposure that runs to the last frame of the project.
constexpr int kOpenEnd = std::numeric_limits<int>::max();

class Layer {
public:
    Layer(std::string name, LayerType type);
    virtual ~Layer() = default;
    uint64_t id() const { return id_; }
    // Stable string id for TimelineModel rows ("L<id>"). Survives renames.
    std::string stableId() const { return "L" + std::to_string(id_); }
    const std::string &name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }
    LayerType type() const { return type_; }
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool locked() const { return locked_; }
    void setLocked(bool l) { locked_ = l; }
    float opacity() const { return opacity_; } // 0..1
    void setOpacity(float o);
    BlendMode blend() const { return blend_; }
    void setBlend(BlendMode b) { blend_ = b; }

private:
    uint64_t id_;
    static uint64_t s_nextId_;
    std::string name_;
    LayerType type_;
    bool visible_{true}, locked_{false};
    float opacity_{1.0f};
    BlendMode blend_{BlendMode::Normal};
};

class PaintLayer : public Layer {
public:
    explicit PaintLayer(std::string name) : Layer(std::move(name), LayerType::Paint) {}
    // Sparse per-frame cells. Frame without key owns nothing (shows hold).
    // cellAt() is the stack; deviceAt() is the device tools paint into — the
    // cell's ACTIVE layer — which is what the tool/selection code keeps using.
    Cell *cellAt(int frame, bool createIfMissing);
    const Cell *cellAt(int frame) const;
    // Replace the cell at frame (no span bookkeeping; callers own exposure).
    void assignCell(int frame, std::unique_ptr<Cell> cell);
    PaintDevice *deviceAt(int frame, bool createIfMissing);
    const PaintDevice *deviceAt(int frame) const;
    bool hasKeyAt(int frame) const;
    void setKeyAt(int frame, bool hasKey); // create blank cell or remove
    std::vector<int> keys() const;
    // --- exposure (cells) ---
    // Every key owns an inclusive span [key, spanEnd]. Spans are stored, not
    // derived: resizing cell A never resizes cell B. A span of kOpenEnd runs
    // to the end of the project; a span may fall short of the next key, which
    // leaves a GAP (no exposure there).
    int spanEnd(int key) const;          // inclusive; kOpenEnd when open
    int spanEndClamped(int key, int maxFrame) const;
    void setSpanEnd(int key, int end);   // >= key, never past the next key
    bool spanOpen(int key) const { return spanEnd(key) == kOpenEnd; }
    // Deep-copy the whole cell (stack + pixels) from one key to another
    // (duplicate/paste). Creates dst key if missing. No-op if src is absent.
    // The exposure length travels with the content.
    void copyDeviceContent(int fromKey, int toKey);
    // Move the whole cell (stack + exposure) from one key to another. Fails
    // (false) when the destination key exists and overwrite==false. Source
    // key is removed.
    bool moveDeviceContent(int fromKey, int toKey, bool overwrite = false);

    // --- transform keyframes (formerly the adjustment layer's params) ---
    // Keyframes live DIRECTLY on the paint layer: anchor/position/scale/
    // rotation/opacity channels, evaluated by the compositor exactly the way
    // the old adjustment layer evaluated them. An empty block is the identity,
    // so a layer without keys composites bit-identically to before.
    ParamBlock &params() { return params_; }
    const ParamBlock &params() const { return params_; }
    bool animated() const { return !params_.empty(); }
    // Source rect = the canvas; the anchor and corner parameters are
    // normalized against it.
    Mat3 matrixFor(const Rect &src, double frame, float *opacityOut = nullptr) const {
        return params_.matrixFor(src, frame, opacityOut);
    }

    // Key exposed at `frame`, or -1 when nothing is exposed there (gap).
    int exposureAt(int frame) const;
    // Shift every key >= from (and its span) by delta, dropping keys pushed
    // past maxFrame. Returns how many keys moved or left the range.
    int shiftFrom(int from, int delta, int maxFrame);
private:
    std::map<int, std::unique_ptr<Cell>> cells_; // key -> that cell's layer stack
    std::map<int, int> spanEnd_; // key -> inclusive end (kOpenEnd = open)
    ParamBlock params_;
};

class GroupLayer : public Layer {
public:
    explicit GroupLayer(std::string name) : Layer(std::move(name), LayerType::Group) {}
    void addChild(std::shared_ptr<Layer> l) { children_.push_back(std::move(l)); }
    void removeChild(size_t i);
    const std::vector<std::shared_ptr<Layer>> &children() const { return children_; }
    std::vector<std::shared_ptr<Layer>> &children() { return children_; }
private:
    std::vector<std::shared_ptr<Layer>> children_;
};

// Camera layer: Clip-Studio-style 2D camera for camera control. Industry
// standard semantics — a camera owns keyframed transform parameters
// (pan/zoom/rotate + opacity) and a scope describing WHAT it frames:
//
//   Layer  -> one PaintLayer (created from the layer's right-click menu)
//   Folder -> one GroupLayer folder as a unit (created from the folder menu)
//   Global -> the final composite (reframe the whole shot)
//
// The camera renders nothing itself and owns no cells. Anchors normalize
// against the CANVAS (the shot), never against content bounds, so the pivot
// cannot drift when ink grows. An empty/identity camera is inert and costs
// nothing (the compositor skips it via isIdentityAt, like paint keyframes).
class CameraLayer : public Layer {
public:
    explicit CameraLayer(std::string name, CameraScope scope = CameraScope::Layer,
                         uint64_t targetId = 0)
        : Layer(std::move(name), LayerType::Camera), scope_(scope), targetId_(targetId) {}

    CameraScope scope() const { return scope_; }
    void setScope(CameraScope s) { scope_ = s; }
    // Target layer/folder id (Layer::id()). 0 = unlinked/global.
    uint64_t targetId() const { return targetId_; }
    void setTargetId(uint64_t id) { targetId_ = id; }

    ParamBlock &params() { return params_; }
    const ParamBlock &params() const { return params_; }
    bool animated() const { return !params_.empty(); }
    bool activeAt(int frame) const {
        return visible() && !params_.empty() && !params_.isIdentityAt(double(frame));
    }
    Mat3 matrixFor(const Rect &canvas, double frame, float *opacityOut = nullptr) const {
        return params_.matrixFor(params_.frameOr(canvas), frame, opacityOut);
    }

private:
    CameraScope scope_{CameraScope::Layer};
    uint64_t targetId_{0};
    ParamBlock params_;
};

} // namespace ink
