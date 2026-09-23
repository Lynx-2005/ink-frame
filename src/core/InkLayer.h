#pragma once
// Layer stack + keyframed raster frames. Production-grade:
// - paint/group layers, opacity, visibility, blend, lock
// - each paint layer owns map<frame, PaintDevice> (sparse holds)
// - active frame resolution (hold semantics in animation layer)
// - projection = recursive composite for a given frame
#include "core/InkPaintDevice.h"
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ink {

// Sentinel for an exposure that runs to the last frame of the project.
constexpr int kOpenEnd = std::numeric_limits<int>::max();

enum class LayerType { Paint, Group };
enum class BlendMode { Normal, Multiply, Screen, Erase };

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
    // Sparse per-frame devices. Frame without key owns nothing (shows hold).
    PaintDevice *deviceAt(int frame, bool createIfMissing);
    const PaintDevice *deviceAt(int frame) const;
    bool hasKeyAt(int frame) const;
    void setKeyAt(int frame, bool hasKey); // create blank or remove
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
    // Deep-copy device content from one key to another (duplicate/paste).
    // Creates dst key if missing. No-op if src has no device. The exposure
    // length travels with the content.
    void copyDeviceContent(int fromKey, int toKey);
    // Move device content (and its exposure) from one key to another. Fails
    // (false) when the destination key exists and overwrite==false. Source
    // key is removed.
    bool moveDeviceContent(int fromKey, int toKey, bool overwrite = false);
    // Key exposed at `frame`, or -1 when nothing is exposed there (gap).
    int exposureAt(int frame) const;
    // Shift every key >= from (and its span) by delta, dropping keys pushed
    // past maxFrame. Returns how many keys moved or left the range.
    int shiftFrom(int from, int delta, int maxFrame);
private:
    std::map<int, std::unique_ptr<PaintDevice>> frames_;
    std::map<int, int> spanEnd_; // key -> inclusive end (kOpenEnd = open)
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

} // namespace ink
