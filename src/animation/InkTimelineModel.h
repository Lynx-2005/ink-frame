#pragma once
// Headless timeline data model (no Qt). Qt docks in src/ui wrap this.
// Production, Clip-Studio-style: tracks ALWAYS live inside folders.
// Even a single-layer project has ≥1 folder. Folders are first-class:
// expand/collapse, color spine, rename, reorder.
//
// A CELL is one key plus its explicit exposure span [start, spanEnd]. Spans are
// stored per cell (mirrored from PaintLayer), never derived from the next key:
// shrinking one cell cannot resize another. A span may stop short of the next
// key, which leaves a gap with no exposure. -1 means "exposes to the last
// frame". A track with no key is an EMPTY track (no cell at all).
#include "animation/InkKeyframe.h"
#include <string>
#include <vector>

namespace ink {

struct TimelineRow {
    std::string layerId;
    std::string layerName;
    RasterChannel frames;
    // key (cell start) -> inclusive exposure end; -1 = to the last frame
    std::map<int, int> spanEnd;
    ScalarChannel opacity;
    bool showThumbs{false};
    int colorOverride{-1}; // -1 = folder hue; else clip palette index
};

// One cell of a track: inclusive frame range plus the track-local ordinal
// (0-based; the UI shows index+1).
struct CellSpan {
    int start{0};
    int end{0};
    int index{0};
};

struct TimelineFolder {
    std::string folderId;
    std::string folderName;
    bool expanded{true};
    int colorIndex{0};
    std::vector<TimelineRow> tracks;
};

class TimelineModel {
public:
    // --- folders (mandatory, CSP-style) ---
    // Returns id of new folder ("F<n>"). Names are user-chosen; no hardcodes.
    std::string addFolder(const std::string &name);
    bool removeFolder(const std::string &folderId);
    bool renameFolder(const std::string &folderId, const std::string &name);
    void setFolderExpanded(const std::string &folderId, bool expanded);
    TimelineFolder *folder(const std::string &folderId);
    const TimelineFolder *folder(const std::string &folderId) const;
    const std::vector<TimelineFolder> &folders() const { return folders_; }
    std::vector<TimelineFolder> &folders() { return folders_; }
    // First folder, or "" if none (UI must create one — mandatory rule).
    std::string defaultFolderId() const;
    int visibleRowCount() const; // folder headers + expanded tracks

    // --- tracks ---
    // Legacy: addLayer without folder lands in default folder (creating
    // "Folder 1" if needed so single-track projects still have a folder).
    void addLayer(const std::string &id, const std::string &name);
    void addTrack(const std::string &folderId, const std::string &id,
                  const std::string &name);
    bool removeLayer(const std::string &id);
    bool moveTrack(const std::string &id, const std::string &toFolderId, int toIndex = -1);
    TimelineRow *row(const std::string &id);
    const TimelineRow *row(const std::string &id) const;
    std::string folderOfTrack(const std::string &trackId) const;

    int currentFrame() const { return current_; }
    void setCurrentFrame(int f);
    int frameCount() const { return frameCount_; }
    void setFrameCount(int n) { if (n>=1&&n<=100000) frameCount_ = n; }
    int fps() const { return fps_; }
    void setFps(int f) { if (f>=1&&f<=120) fps_ = f; }

    // Key ops used by dock + canvas
    void setKey(const std::string &layerId, int frame);
    void clearKey(const std::string &layerId, int frame);
    bool moveKey(const std::string &layerId, int from, int to);
    int activeKey(const std::string &layerId, int frame) const;

    // --- cells (one key + its own exposure; empty track => empty vector) ---
    std::vector<CellSpan> cells(const std::string &layerId) const;
    int cellCount(const std::string &layerId) const;
    // Ordinal of the cell exposing `frame`, or -1 when nothing is exposed.
    int cellIndexAt(const std::string &layerId, int frame) const;
    // Cell start exposing `frame` (its identity), or -1.
    int cellStartAt(const std::string &layerId, int frame) const;
    // Exposure of one cell. end < 0 = to the last frame; clipped to the next
    // key so cells never overlap (resizing is still purely local).
    int cellEnd(const std::string &layerId, int start) const;
    void setCellEnd(const std::string &layerId, int start, int end);
    int cellLength(const std::string &layerId, int start) const;

    // Legacy flattened view (all tracks, folder order). Kept for compat.
    // Rebuilt lazily from folders.
    const std::vector<TimelineRow> &rows() const;

    // Camera scalar channel. Data only: no timeline row is rendered for it
    // (the camera track was removed from the UI). Kept for the future
    // position/zoom track and for project round-tripping.
    ScalarChannel &cameraKeys() { return camera_; }
    const ScalarChannel &cameraKeys() const { return camera_; }

private:
    void rebuildFlat() const;
    std::vector<TimelineFolder> folders_;
    ScalarChannel camera_;
    mutable std::vector<TimelineRow> flatCache_;
    mutable bool flatDirty_{true};
    int current_{0}, frameCount_{120}, fps_{24};
    int nextFolder_{1};
};

} // namespace ink
