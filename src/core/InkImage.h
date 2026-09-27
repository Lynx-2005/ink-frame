#pragma once
// Image = canvas size + fps + layer tree + projection.
// Production: frame-accurate projection with per-layer hold resolution,
// opacity, blend, visibility; dirty-rect union for canvas invalidation.
#include "core/InkLayer.h"
#include <map>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

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

    // Guards the layer tree AND every cell stack in it. Readers (projection —
    // including the frame cache's worker threads — and canvas paint) take a
    // shared lock; anything that edits structure takes a unique lock through
    // Document::EditGuard. Without this, a worker could walk a cell stack
    // while the UI is adding/removing layers in it (a real crash, not theory).
    std::shared_mutex &treeMutex() const { return treeMutex_; }

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

    // --- per-cell layer stacks (InkCell.h) ---
    // Composite one cell's stack into out (no clearing: accumulates like the
    // layer walk does). A group with its own opacity/blend is isolated through
    // a scratch device so it reads as one unit.
    void compositeCell(const Cell &cell, PaintDevice &out) const;
    void compositeCellRect(const Cell &cell, const Rect &rect, PaintDevice &out) const;
    // Composite the cell at `key` of a track into out (cleared first, track
    // chrome NOT applied) — what the timeline thumbnails show. Returns the
    // content extent clamped to the canvas.
    Rect cellComposite(const PaintLayer &pl, int key, PaintDevice &out) const;

    // --- Clip-Studio-style folder/track API (folders mandatory) ---
    // Tracks (PaintLayer) must live inside a GroupLayer folder. Root-level
    // paint layers are legacy and get adopted into a folder on touch.
    GroupLayer *createFolder(const std::string &name);
    PaintLayer *createTrack(GroupLayer *folder, const std::string &name);
    // Camera layers (2D camera): created from the layer menu (scoped to one
    // track) or the folder menu (scoped to the whole folder), or global.
    // Storage: layer cameras live inside the target's folder, right after the
    // target track; folder cameras live as the folder's FIRST child. Either
    // way they render nothing themselves — they transform their scope.
    // `index` pins the position inside `folder` (<0 appends); callers pass the
    // placement so append-order is never user-visible.
    CameraLayer *createCamera(GroupLayer *folder, const std::string &name,
                              CameraScope scope, uint64_t targetId = 0, int index = -1);
    std::string autoCameraName();
    // Auto names for UI-created items: "Folder N" / "Layer N" with the first
    // unused N, so boot names never collide with newly added ones.
    std::string autoFolderName();
    std::string autoLayerName();
    GroupLayer *ensureFolder(); // first folder, or creates "Folder 1"
    GroupLayer *findFolderOf(const Layer *track);
    GroupLayer *findFolderByName(const std::string &name);
    void collectFolders(std::vector<GroupLayer*> &out);
    void collectFolders(std::vector<const GroupLayer*> &out) const;
    // All paint layers in tree order (bottom..top)
    void collectPaintLayers(std::vector<PaintLayer*> &out);
    void collectPaintLayers(std::vector<const PaintLayer*> &out) const;
    // All camera layers in tree order (bottom..top)
    void collectCameras(std::vector<CameraLayer*> &out);
    void collectCameras(std::vector<const CameraLayer*> &out) const;

    // A paint layer by id, or nullptr.
    PaintLayer *paintLayerById(uint64_t id);
    const PaintLayer *paintLayerById(uint64_t id) const;
    // A camera layer by id, or nullptr. Any layer by id.
    CameraLayer *cameraById(uint64_t id);
    const CameraLayer *cameraById(uint64_t id) const;
    Layer *layerById(uint64_t id);
    const Layer *layerById(uint64_t id) const;
    GroupLayer *folderById(uint64_t id);
    const GroupLayer *folderById(uint64_t id) const;
    bool deleteCamera(CameraLayer *cam);

    // --- layer-tree structure ops (timeline grouping) ---
    // Move `members` into a NEW folder created next to the topmost member's
    // folder. Member order follows current tree order. Empty input => nullptr.
    // Works on any layer kind.
    GroupLayer *groupLayers(const std::vector<Layer*> &members,
                            const std::string &name = {});
    // Dissolve a folder: paint children move into the neighbouring folder
    // (the one above it, else the one below), nested groups are hoisted to
    // root. Refuses when it is the only folder (folders are mandatory).
    bool ungroupFolder(GroupLayer *g);
    // Move any layer into another folder. index < 0 appends.
    bool moveLayer(Layer *l, GroupLayer *to, int index = -1);

private:
    // One paint layer's resolved contribution to a projection, keyed by
    // the layer's own id. Paint layers carry their transform keyframes
    // DIRECTLY (InkLayer.h), so "adjustment" here means "this layer is
    // animated at this frame".
    struct AdjustRef { const PaintLayer *layer{nullptr}; };
    // Hash maps (were std::map): the incremental path rebuilds these per dab
    // and looks each layer up by id. Order never mattered — projectLayer
    // looks up by id, and the grown-rect union is order-independent.
    using AdjustMap = std::unordered_map<uint64_t, AdjustRef>;
    // One camera's resolved contribution at this frame. Keyed by scope target
    // (layer id / folder id); globals are chained in tree order. First match
    // in tree order wins per target so overlapping cameras stay deterministic.
    struct CameraRef { const CameraLayer *cam{nullptr}; };
    using CameraMap = std::unordered_map<uint64_t, CameraRef>;

    // Every paint layer whose parameters are NON-IDENTITY at this frame, in
    // tree order, keyed by layer id. A block whose parameters all evaluate to
    // their defaults is skipped here: compositing it would resample a
    // bit-identical copy of the track, and the incremental walk would lose its
    // dirty-rect contract. A block whose parameters all evaluate to their defaults is
    // skipped here: compositing it would resample a bit-identical copy of the
    // track, and the incremental walk would lose its dirty-rect contract. This
    // filter is what makes adding an adjustment layer (and every frame an
    // animation passes through rest) cost nothing.
    void collectAdjustments(int frame, AdjustMap &out) const;
    // Every camera layer that is VISIBLE and NON-IDENTITY at this frame, split
    // by scope. Invisible/identity/unlinked-target cameras are inert (no cost).
    // First in tree order wins per target.
    void collectCameraLayers(int frame, CameraMap &layerCams, CameraMap &folderCams,
                             std::vector<CameraRef> &globalCams) const;
    // Single-walk form of the two above: project()/projectRect() used to walk
    // the whole tree twice per call (per dab on the stroke path). One walk
    // visits each node once and preserves both methods' exact semantics.
    void collectFrame(int frame, AdjustMap &adj, CameraMap &layerCams, CameraMap &folderCams,
                      std::vector<CameraRef> &globalCams) const;
    // Composite one paint layer's exposed cell into out, animated by `adj` and
    // then reframed by `cam` (layer-scoped camera, may be null).
    void compositeTrack(const PaintLayer &pl, int frame, PaintDevice &out,
                        const AdjustRef *adj, const CameraRef *cam = nullptr) const;
    void projectLayer(const Layer &l, int frame, PaintDevice &out, const AdjustMap &adj,
                      const CameraMap &layerCams, const CameraMap &folderCams) const;
    // Incremental walk. `adj` is empty whenever projectRect can guarantee the
    // rect covers everything an animated track can move in or out of; a
    // singular or un-invertible map takes the full-rebuild fallback instead.
    // Layer-scoped cameras walk incrementally too (each warps exactly one
    // track, so the grown rect covers them like any other map). Folder and
    // global cameras still take the full-rebuild path: they reframe whole
    // composites, whose pixels can legitimately come from anywhere.
    void projectLayerRect(const Layer &l, int frame, const Rect &rect, PaintDevice &out,
                          const AdjustMap &adj, const CameraMap &layerCams,
                          const CameraMap &folderCams) const;
    // The animated path of the incremental walk: composite the cell clipped to
    // the INVERSE-mapped rect, resample into that rect, stamp. Values identical
    // to a full project() restricted to the rect. `cam` is the track's
    // layer-scoped camera (null when none): its map applies AFTER the layer's
    // own keyframes (total = camera * layer, exactly like compositeTrack), so
    // the grown rect must cover the combined map, not just the layer's.
    void compositeTrackRect(const PaintLayer &pl, int frame, const Rect &rect, PaintDevice &out,
                            const AdjustRef *adj, const CameraRef *cam = nullptr) const;
    int w_, h_, fps_, frameCount_{120};
    std::unique_ptr<GroupLayer> root_;
    mutable std::shared_mutex treeMutex_;
};

} // namespace ink
