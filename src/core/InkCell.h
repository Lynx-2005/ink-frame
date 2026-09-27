#pragma once
// Per-cell layer stack.
//
// A timeline CELL (one key plus its exposure span on a PaintLayer track) owns
// its OWN stack of CellLayers: paint layers (one PaintDevice each) and group
// layers that hold more of either. Nothing is shared between cells — two cells
// of the same track, or of different tracks, never point at the same device.
//
// This is a separate concept from the timeline tree: GroupLayer/PaintLayer
// ("folder"/"track") structure the whole animation; CellLayer structures ONE
// drawing. Cell stack ops mirror Image's folder/track ops so the two read the
// same way in the UI (grouping, stack order, last-paint-layer guard).
#include "core/InkLayerTypes.h" // LayerType, BlendMode
#include "core/InkPaintDevice.h"
#include <memory>
#include <string>
#include <vector>

namespace ink {

class CellLayer {
public:
    CellLayer(std::string name, LayerType type);
    uint64_t id() const { return id_; }
    // Stable string id for UI selection state ("C<id>"). Survives renames.
    std::string stableId() const { return "C" + std::to_string(id_); }
    const std::string &name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }
    LayerType type() const { return type_; }
    bool isPaint() const { return type_ == LayerType::Paint; }
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    bool locked() const { return locked_; }
    void setLocked(bool l) { locked_ = l; }
    float opacity() const { return opacity_; } // 0..1
    void setOpacity(float o);
    BlendMode blend() const { return blend_; }
    void setBlend(BlendMode b) { blend_ = b; }

    // Paint layers own exactly one device: the cell IS the frame dimension.
    PaintDevice *device() { return device_.get(); }
    const PaintDevice *device() const { return device_.get(); }

    // Group layers own children, bottom..top (later index composites on top).
    void addChild(std::shared_ptr<CellLayer> l) { children_.push_back(std::move(l)); }
    bool removeChild(size_t i);
    std::vector<std::shared_ptr<CellLayer>> &children() { return children_; }
    const std::vector<std::shared_ptr<CellLayer>> &children() const { return children_; }

    // Any visible pixel content anywhere at/below this layer? (blank test)
    bool hasContent() const;

    // Deep copy: fresh ids, copied pixels. Duplicate/paste/mirror use this so
    // no two cells ever share a device.
    std::shared_ptr<CellLayer> clone() const;

private:
    uint64_t id_;
    static uint64_t s_nextId_;
    std::string name_;
    LayerType type_;
    bool visible_{true}, locked_{false};
    float opacity_{1.0f};
    BlendMode blend_{BlendMode::Normal};
    std::unique_ptr<PaintDevice> device_;              // paint kind
    std::vector<std::shared_ptr<CellLayer>> children_; // group kind
};

// The layer stack of one cell. Boots with a single paint layer so drawing
// behaves exactly as it did before stacks existed.
class Cell {
public:
    Cell();

    std::vector<std::shared_ptr<CellLayer>> &layers() { return layers_; }
    const std::vector<std::shared_ptr<CellLayer>> &layers() const { return layers_; }
    // Which paint layer tools paint into. Persisted per cell so switching
    // frames remembers the selection.
    uint64_t activeId() const { return activeId_; }
    void setActiveId(uint64_t id) { activeId_ = id; }

    CellLayer *find(uint64_t id);
    const CellLayer *find(uint64_t id) const;
    std::shared_ptr<CellLayer> findShared(uint64_t id) const;
    // Vector that holds `l` (top-level layers or a group's children), or null.
    std::vector<std::shared_ptr<CellLayer>> *parentOf(const CellLayer *l);
    std::vector<std::shared_ptr<CellLayer>> *parentOf(const CellLayer *l, CellLayer **holder);
    // Active layer when it is still a paint layer, else the topmost paint
    // layer, else null.
    CellLayer *activePaintLayer();
    const CellLayer *activePaintLayer() const;

    // Inherited state: hide and lock are set per layer, but a group's hide or
    // lock also applies to everything under it. These answer "what does this
    // layer do right now", which is what painting and the panel care about.
    bool lockedThrough(const CellLayer *l) const;  // itself or a group above it
    bool visibleThrough(const CellLayer *l) const; // itself and every group above

    // --- structure (mirrors Image's folder/track ops, cell scope) ---
    // `above` = insert directly above that layer (same parent); null = top.
    CellLayer *addLayer(const std::string &name, CellLayer *above);
    CellLayer *addGroup(const std::string &name, CellLayer *above);
    // Copy of `l` inserted directly above it (fresh ids, copied pixels).
    CellLayer *duplicate(const CellLayer *l);
    // Remove a layer. Refuses to remove the last paint layer of the cell
    // (a cell without a paint layer could not be drawn on).
    bool remove(CellLayer *l);
    // Would removing `l` leave the cell without any paint layer?
    bool canRemove(const CellLayer *l) const;
    // Reorder within the parent: delta +1 = one step toward the front (up in
    // the panel, higher composite index), -1 = one step toward the back.
    bool move(CellLayer *l, int delta);
    // Reparent into `group` (null = top level), appended on top.
    bool moveToGroup(CellLayer *l, CellLayer *group);
    // Bundle layers into a NEW group placed where the topmost member was.
    CellLayer *group(const std::vector<CellLayer*> &sel, const std::string &name);
    // Dissolve a group: children take its place, in order.
    bool ungroup(CellLayer *g);

    bool canRemovePaint() const;      // more than one paint layer exists
    int paintLayerCount() const;
    // Any pixel content anywhere in this stack? (blank vs drawn cell)
    bool hasContent() const;
    std::string autoLayerName() const; // first unused "Layer N" in this cell

    // Deep copy of another stack (fresh ids, copied pixels), used by paste.
    void copyFrom(const Cell &src);

    // --- low-level slots (undo/redo restores exact positions) ---
    // parentId 0 = top level; index < 0 or > size appends.
    bool insertAt(std::shared_ptr<CellLayer> l, uint64_t parentId, int index);
    // Detach without destroying; returns the held pointer (null if absent).
    std::shared_ptr<CellLayer> detach(uint64_t id);
    // Slot of a layer in the tree; false when absent.
    bool slotOf(const CellLayer *l, uint64_t &parentId, int &index) const;

private:
    std::vector<std::shared_ptr<CellLayer>> layers_;
    uint64_t activeId_{0};
};

} // namespace ink
