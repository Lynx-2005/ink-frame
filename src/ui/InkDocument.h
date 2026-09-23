#pragma once
// InkDocument: Qt-facing owner of engine objects.
// Runs FrameCache render fn on Image::project, keeps TimelineModel in sync
// with PaintLayer keys, owns UndoStack + StrokeQueue.
// Clip-Studio-style rule: tracks ALWAYS live inside folders. The document
// boots with Folder 1 + one EMPTY Layer 1 (no cell, no phantom exposure) and
// creation helpers enforce the folder-first flow (user names everything; no
// hardcoded project names). Cells appear when the user adds one or paints.
#include "animation/InkAnimation.h"
#include "animation/InkTimelineModel.h"
#include "core/InkImage.h"
#include "core/InkStroke.h"
#include <memory>
#include <string>
#include <vector>

namespace ink {

class Document {
public:
    Document(int w = 1920, int h = 1080, int fps = 24);
    Image &image() { return *image_; }
    const Image &image() const { return *image_; }
    UndoStack &undo() { return undo_; }
    TimelineModel &timeline() { return timeline_; }
    const TimelineModel &timeline() const { return timeline_; }
    FrameCache &frameCache() { return *cache_; }
    PlaybackEngine &playback() { return playback_; }
    OnionSkinConfig &onion() { return onion_; }

    PaintLayer *currentLayer() { return currentLayer_; }
    void setCurrentLayer(PaintLayer *l) { currentLayer_ = l; }
    GroupLayer *currentFolder() { return currentFolder_; }
    void setCurrentFolder(GroupLayer *g) { currentFolder_ = g; }
    int currentFrame() const { return timeline_.currentFrame(); }
    void setCurrentFrame(int f);
    // Project length in frames (1 .. 100000). Keeps image, timeline, playback
    // range and frame cache in step; re-clamps the playhead when shortening.
    bool setFrameCount(int n);
    int frameCount() const { return timeline_.frameCount(); }

    // Folder-first creation (CSP-style). Empty name => auto "Folder N".
    GroupLayer *createFolder(const std::string &name = {});
    // Track creation always targets a folder (defaults to current folder,
    // else first folder). Empty name => auto "Layer N".
    PaintLayer *createTrack(GroupLayer *folder = nullptr, const std::string &name = {});
    // CSP/Krita: new track directly ABOVE the current layer, same folder.
    PaintLayer *createTrackAboveCurrent(const std::string &name = {});
    PaintLayer *duplicateTrack(PaintLayer *src);
    bool deleteTrack(PaintLayer *layer);
    GroupLayer *ensureFolder();

    // --- grouping (timeline gutter multi-select) ---
    // Bundle the given tracks into a NEW folder; empty name => "Folder N".
    GroupLayer *groupSelected(const std::vector<PaintLayer*> &sel, const std::string &name = {});
    // Dissolve a folder into its neighbour (refused when it is the last one).
    bool ungroupFolder(GroupLayer *g);
    bool moveTrackToFolder(PaintLayer *l, GroupLayer *to);
    // Delete a folder with its contents. The last folder can't be removed
    // (folders are mandatory) — its contents are cleared instead.
    bool deleteFolder(GroupLayer *g);
    // Reorder a track inside its folder (delta = -1 up / +1 down, stack order).
    bool moveTrackInFolder(PaintLayer *l, int delta);

    // Ensure a paint key exists at frame (creates undoable key op — simplified)
    PaintDevice *editableDevice(PaintLayer *layer, int frame);
    void touchLayer(PaintLayer *layer, int frame); // invalidate cache + sync timeline

    // --- cells: one key + its own exposure span (independent per cell) ---
    int cellStartAt(PaintLayer *layer, int frame) const; // -1 when nothing exposed
    int cellEnd(PaintLayer *layer, int start) const;     // inclusive, frameCount-aware
    int cellLength(PaintLayer *layer, int start) const;
    // Resize one cell. end < 0 or >= last frame = expose to the end.
    bool setCellEnd(PaintLayer *layer, int start, int end);
    bool setCellLength(PaintLayer *layer, int start, int length);
    int extendCellToNext(PaintLayer *layer, int start);
    int extendCellToEnd(PaintLayer *layer, int start);
    // Trim/grow a cell from its left edge: the start moves, the end stays.
    bool moveCellStart(PaintLayer *layer, int start, int newStart);

    // --- Krita-parity keyframe ops (all end with touchLayer) ---
    struct KeyClipboard {
        // One copied cell: frame offset from the first key, its exposure
        // length, and a deep copy of its pixels.
        struct Entry {
            int offset{0};
            int length{1};
            std::shared_ptr<PaintDevice> device;
        };
        bool valid{false};
        std::string srcTrackId;
        std::vector<Entry> keys;
    };
    // A new cell is ONE frame long; extend it by dragging its right edge or
    // through the Exposure menu.
    bool addBlankKey(PaintLayer *layer, int frame);
    bool duplicateKey(PaintLayer *layer, int srcFrame, int dstFrame, bool overwrite = false);
    int removeKeys(PaintLayer *layer, const std::vector<int> &frames, bool pull = false);
    int insertHold(PaintLayer *layer, int frame, int count = 1); // grow the cell
    int removeHold(PaintLayer *layer, int frame, int count = 1); // shrink the cell
    int shiftKeys(PaintLayer *layer, int fromFrame, int delta);
    int mirrorKeys(PaintLayer *layer, int lo, int hi);
    bool copyKeys(PaintLayer *layer, const std::vector<int> &frames);
    int pasteKeys(PaintLayer *layer, int dstFrame, bool overwrite = false);
    const KeyClipboard &clipboard() const { return clipboard_; }
    PaintLayer *findTrack(const std::string &stableId);

private:
    void syncTimelineFromImage();
    std::unique_ptr<Image> image_;
    UndoStack undo_;
    TimelineModel timeline_;
    std::unique_ptr<FrameCache> cache_;
    PlaybackEngine playback_;
    OnionSkinConfig onion_;
    PaintLayer *currentLayer_{nullptr};
    GroupLayer *currentFolder_{nullptr};
    KeyClipboard clipboard_;
};

} // namespace ink
