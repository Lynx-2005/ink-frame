#pragma once
// Image = canvas size + fps + layer tree + projection.
// Production: frame-accurate projection with per-layer hold resolution,
// opacity, blend, visibility; dirty-rect union for canvas invalidation.
#include "core/InkLayer.h"
#include <memory>

namespace ink {

class Image {
public:
    Image(int w, int h, int fps = 24);
    int width() const { return w_; }
    int height() const { return h_; }
    int fps() const { return fps_; }
    void setFps(int f);
    int frameCount() const { return frameCount_; }
    void setFrameCount(int n);

    GroupLayer &root() { return *root_; }
    const GroupLayer &root() const { return *root_; }

    // Flatten all visible layers at frame into out (out is cleared first).
    // Returns dirty rect (union of layer extents clamped to canvas).
    Rect project(int frame, PaintDevice &out) const;

    // Incremental re-projection of rect only (the buttery-smooth path):
    // clears rect in out, then re-composites every visible layer clipped to
    // rect. Untouched layers cost ~zero: tile sparsity means layers with no
    // content in rect contribute no tiles and no pixel work, so hundreds of
    // layers stay cheap as long as content in rect is small (the stroke case).
    // out must already hold the projection of all OTHER regions.
    // Returns the tile-aligned region actually refreshed (superset of rect);
    // the caller must upload/ghost exactly that region.
    Rect projectRect(int frame, const Rect &rect, PaintDevice &out) const;

    // --- Clip-Studio-style folder/track API (folders mandatory) ---
    // Tracks (PaintLayer) must live inside a GroupLayer folder. Root-level
    // paint layers are legacy and get adopted into a folder on touch.
    GroupLayer *createFolder(const std::string &name);
    PaintLayer *createTrack(GroupLayer *folder, const std::string &name);
    // Auto names for UI-created items: "Folder N" / "Layer N" with the first
    // unused N, so boot names never collide with newly added ones.
    std::string autoFolderName();
    std::string autoLayerName();
    GroupLayer *ensureFolder(); // first folder, or creates "Folder 1"
    GroupLayer *findFolderOf(PaintLayer *track);
    GroupLayer *findFolderByName(const std::string &name);
    void collectFolders(std::vector<GroupLayer*> &out);
    void collectFolders(std::vector<const GroupLayer*> &out) const;
    // All paint layers in tree order (bottom..top)
    void collectPaintLayers(std::vector<PaintLayer*> &out);
    void collectPaintLayers(std::vector<const PaintLayer*> &out) const;

    // --- layer-tree structure ops (timeline grouping) ---
    // Move `members` into a NEW folder created next to the topmost member's
    // folder. Member order follows current tree order. Empty input => nullptr.
    GroupLayer *groupTracks(const std::vector<PaintLayer*> &members,
                            const std::string &name = {});
    // Dissolve a folder: paint children move into the neighbouring folder
    // (the one above it, else the one below), nested groups are hoisted to
    // root. Refuses when it is the only folder (folders are mandatory).
    bool ungroupFolder(GroupLayer *g);
    // Move a track into another folder. index < 0 appends.
    bool moveTrack(PaintLayer *l, GroupLayer *to, int index = -1);

private:
    void projectLayer(const Layer &l, int frame, PaintDevice &out) const;
    void projectLayerRect(const Layer &l, int frame, const Rect &rect, PaintDevice &out) const;
    int w_, h_, fps_, frameCount_{120};
    std::unique_ptr<GroupLayer> root_;
};

} // namespace ink
