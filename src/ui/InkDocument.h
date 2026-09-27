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
#include "core/InkAdjustmentLayer.h"
#include "core/InkImage.h"
#include "core/InkSelection.h"
#include "core/InkStroke.h"
#include <memory>
#include <shared_mutex>
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
    // One selection per document (Krita keeps it on the image): painting and
    // filling are clipped by it, selection tools edit it.
    Selection &selection() { return selection_; }

    PaintLayer *currentLayer() { return currentLayer_; }
    void setCurrentLayer(PaintLayer *l) { currentLayer_ = l; }
    GroupLayer *currentFolder() { return currentFolder_; }
    void setCurrentFolder(GroupLayer *g) { currentFolder_ = g; }
    // Compatibility view of the current layer's keyframes. There is no separate
    // adjustment selection any more: the working layer is always a paint layer,
    // and this facade simply exposes its params to the panel/graph/tests.
    AdjustmentLayer *currentAdjustment();
    void setCurrentAdjustment(AdjustmentLayer *a);

    // --- thread safety for the layer tree ---
    // The frame cache renders on worker threads through Image::project. Any
    // edit of layers, cells or a cell's layer stack must hold one of these for
    // its whole duration, so a worker can never walk a half-updated stack.
    // Nested ops reuse the outer lock, and every Document op takes one — this
    // is for widgets that edit a layer directly (eye/lock/opacity/blend/name).
    class EditGuard {
    public:
        explicit EditGuard(Document &doc) : doc_(doc) { doc_.beginEdit(); }
        ~EditGuard() { doc_.endEdit(); }
        EditGuard(const EditGuard &) = delete;
        EditGuard &operator=(const EditGuard &) = delete;

    private:
        Document &doc_;
    };
    EditGuard editGuard() { return EditGuard(*this); }

    // Undo/redo with the tree lock held, so an undone edit cannot race a
    // render in flight. (The UndoStack itself is still reachable through
    // undo()/redo() accessors; these are the entry points the UI uses.)
    void undoEdit();
    void redoEdit();
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

    // --- camera layers (Clip-Studio-style 2D camera) ---
    // Created from the LAYER right-click menu (scoped to that track) or the
    // FOLDER right-click menu (scoped to the whole folder). Global cameras
    // reframe the final composite. Empty name => auto "Camera N".
    CameraLayer *createCameraForLayer(PaintLayer *target, const std::string &name = {});
    CameraLayer *createCameraForFolder(GroupLayer *folder, const std::string &name = {});
    CameraLayer *createGlobalCamera(const std::string &name = {});
    bool deleteCamera(CameraLayer *cam);
    bool renameCamera(CameraLayer *cam, const std::string &name);
    CameraLayer *findCamera(const std::string &stableId);
    void collectCameraLayers(std::vector<CameraLayer*> &out);
    // Camera keyframe ops (all undoable, one undo step each). Mirrors the
    // paint-layer param API but targets a camera's own ParamBlock.
    bool setCameraKey(CameraLayer *c, ParamId id, int frame, double value,
                      Interp interp = Interp::Bezier, bool updateExisting = true);
    bool removeCameraKey(CameraLayer *c, ParamId id, int frame);
    bool toggleCameraKey(CameraLayer *c, ParamId id, int frame);
    bool addCameraKey(CameraLayer *c, ParamId id, int frame);
    bool moveCameraKey(CameraLayer *c, ParamId id, int from, int to);
    bool setCameraKeyValue(CameraLayer *c, ParamId id, int frame, double value);
    bool setCameraKeyInterp(CameraLayer *c, ParamId id, int frame, Interp i);
    bool setCameraKeyHandles(CameraLayer *c, ParamId id, int frame, double outInfl,
                             double outSpeed, double inInfl, double inSpeed);
    bool setCameraStaticValue(CameraLayer *c, ParamId id, double value);
    bool setCameraValue(CameraLayer *c, ParamId id, double value, int frame);
    bool applyCameraPreset(CameraLayer *c, int frame, Interp mode, bool easeIn, bool easeOut);
    int keyCameraAt(CameraLayer *c, int frame);
    int removeCameraKeysAt(CameraLayer *c, int frame);
    int prevCameraKey(CameraLayer *c, int frame);
    int nextCameraKey(CameraLayer *c, int frame);
    // Camera tool helpers: pan/zoom/rotate the camera like a real 2D camera
    // tool (one undo step per drag via the record API below). Uniform zoom
    // keeps X/Y locked (industry default); rotate is in degrees.
    void beginCameraRecord(const std::string &label, CameraLayer *c);
    void recordCameraValue(CameraLayer *c, ParamId id, double value, int frame);
    void endCameraRecord();
    void cancelCameraRecord();
    bool cameraRecordActive(CameraLayer *c) const;
    bool cameraPanBy(CameraLayer *c, int frame, double dx, double dy);
    bool cameraZoomBy(CameraLayer *c, int frame, double factor, bool uniform = true);
    bool cameraRotateBy(CameraLayer *c, int frame, double degrees);
    void touchCamera(CameraLayer *c);

    // --- grouping (timeline gutter multi-select) ---
    // Bundle the given layers into a NEW folder; empty name => "Folder N".
    // Any layer kind: adjustment layers group like paint layers.
    GroupLayer *groupSelected(const std::vector<Layer*> &sel, const std::string &name = {});
    // Dissolve a folder into its neighbour (refused when it is the last one).
    bool ungroupFolder(GroupLayer *g);
    bool moveLayerToFolder(Layer *l, GroupLayer *to);
    // Delete a folder with its contents. The last folder can't be removed
    // (folders are mandatory) — its contents are cleared instead.
    bool deleteFolder(GroupLayer *g);
    // Reorder a track inside its folder (delta = -1 up / +1 down, stack order).
    bool moveLayerInFolder(Layer *l, int delta);

    // Ensure a paint key exists at frame (creates undoable key op — simplified)
    // When auto-key is OFF and nothing is exposed at frame, returns nullptr
    // instead of creating a cell (Block + hint UX).
    PaintDevice *editableDevice(PaintLayer *layer, int frame);
    bool autoKeyEnabled() const { return autoKey_; }
    void setAutoKeyEnabled(bool on) { autoKey_ = on; }
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
        // length, and a deep copy of its whole layer stack.
        struct Entry {
            int offset{0};
            int length{1};
            std::shared_ptr<Cell> cell;
        };
        bool valid{false};
        std::string srcTrackId;
        std::vector<Entry> keys;
    };
    // A new cell is ONE frame long; extend it by dragging its right edge or
    // through the Exposure menu.
    bool addBlankKey(PaintLayer *layer, int frame);
    // Insert a new blank 1-frame cell at `atFrame`: every key at/after it ripples
    // one frame right, content and exposure travelling together. A frame inside a
    // cell's hold snaps to just after that cell, so an exposure is never split
    // into a blank gap; the project grows a frame when the ripple needs the room.
    bool insertCell(PaintLayer *layer, int atFrame);
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
    // Any layer kind by stable id ("L<id>").
    Layer *findLayer(const std::string &stableId);
    // A compatibility VIEW of a paint layer's keyframes (InkAdjustmentLayer.h).
    // `stableId` may be the paint layer's own id, or one of the view ids.
    AdjustmentLayer *findAdjustment(const std::string &stableId);

    // --- keyframes on the paint layer (compatibility API) -------------------
    // The old adjustment layer is gone; these hand out a VIEW of the target
    // paint layer's own params. Creation links the view to `target` and never
    // touches the layer tree. `select` only re-points the working layer.
    AdjustmentLayer *createAdjustmentLayer(PaintLayer *target = nullptr, bool select = true);
    // The existing view for `target`, else one is created.
    AdjustmentLayer *ensureAdjustmentFor(PaintLayer *target);
    AdjustmentLayer *adjustmentFor(PaintLayer *target);
    // Re-pointing the view is a no-op in the new model (keyframes live on the
    // layer itself), kept so call sites keep compiling.
    bool linkAdjustment(AdjustmentLayer *a, PaintLayer *target);
    // Clears the layer's keyframes (one undo step); the view object survives.
    bool deleteAdjustment(AdjustmentLayer *a);
    // The active span concept is gone; these are no-ops kept for compilation.
    bool setAdjustmentSpan(AdjustmentLayer *a, int start, int end);
    bool renameAdjustment(AdjustmentLayer *a, const std::string &name);
    void previewAdjustmentBar(AdjustmentLayer *a, int start, int end, int keyDelta,
                              const ParamBlock &base);
    void commitAdjustmentBar(AdjustmentLayer *a, int origStart, int origEnd,
                             const ParamBlock &base);
    void abortAdjustmentBar(AdjustmentLayer *a, int origStart, int origEnd,
                            const ParamBlock &base);
    // Invalidate every cached frame: a parameter edit can move pixels anywhere.
    void touchAdjustment(AdjustmentLayer *a);
    // Keep the marching ants on keyframed ink (UI thread only): when the
    // selection's track is the current layer, the selection is handed the
    // animation's current matrix to DISPLAY through (its mask stays in device
    // space, where the pixels are stored).
    void syncSelectionTransform();
    void invalidateAdjustmentRange(int lo, int hi);

    // --- keyframe ops (all undoable, all one undo step) ---
    // Insert or update a key. A new Bezier key derives its handles from its
    // neighbours, so keying twice never introduces motion nobody asked for.
    bool setParamKey(AdjustmentLayer *a, ParamId id, int frame, double value,
                     Interp interp = Interp::Bezier, bool updateExisting = true);
    bool removeParamKey(AdjustmentLayer *a, ParamId id, int frame);
    // The stopwatch/diamond toggle: turning animation ON keys the value the
    // parameter ALREADY has at that frame, so the picture never jumps.
    bool toggleParamKey(AdjustmentLayer *a, ParamId id, int frame);
    bool addParamKey(AdjustmentLayer *a, ParamId id, int frame);
    bool moveParamKey(AdjustmentLayer *a, ParamId id, int from, int to);
    bool setParamKeyValue(AdjustmentLayer *a, ParamId id, int frame, double value);
    bool setParamKeyInterp(AdjustmentLayer *a, ParamId id, int frame, Interp i);
    bool setParamKeyHandles(AdjustmentLayer *a, ParamId id, int frame, double outInfl,
                            double outSpeed, double inInfl, double inSpeed);
    bool setParamStaticValue(AdjustmentLayer *a, ParamId id, double value);
    // Write a value the way the record path does: an animated parameter takes a
    // key at `frame`, an un-animated one just changes its static value.
    bool setParamValue(AdjustmentLayer *a, ParamId id, double value, int frame);
    // Apply a preset to every key at `frame`: Linear/Hold change the segment
    // mode; Bezier eases by zeroing the speed on the chosen side (AE's Easy
    // Ease family).
    bool applyKeyPreset(AdjustmentLayer *a, int frame, Interp mode, bool easeIn, bool easeOut);
    // THE header button. Keys the animated parameter set, or the default
    // transform group when nothing is animated yet. Returns keys written.
    int keyCurrentLayer(int frame);
    int removeKeyCurrentLayer(int frame);
    // Previous/next keyed frame on the working layer (-1 when there is none).
    int prevKeyCurrentLayer(int frame);
    int nextKeyCurrentLayer(int frame);
    // Resolves the working layer to the adjustment layer keys go to, creating
    // and linking one when the working layer is a paint layer.
    AdjustmentLayer *keyingTarget();
    // The content bbox a fresh block's reference frame is seeded from:
    // the selection when there is one, else the exposed cell's ink extent,
    // else the canvas. Device space, like the transform tool's lift.
    Rect contentFrameFor(PaintLayer *target, int frame);
    // Tool recording: one undo step spanning a whole drag.
    void beginParamRecord(const std::string &label, AdjustmentLayer *a);
    void recordParamValue(ParamId id, double value, int frame);
    void endParamRecord();
    bool recordActive() const { return record_.active; }
    // Abandon a recorded drag (Esc): the block goes back to what it was when the
    // record began and no undo entry is pushed.
    void cancelParamRecord();

    // NOTE: rows. A ROW rebuild (syncTimelineFromImage) is only for STRUCTURAL
    // changes — add/delete/link/reorder — and lives at those call sites, not in
    // touchAdjustment: the timeline reads bars, diamonds and thumbnails live at
    // paint time, so a parameter edit costs one repaint, never a tree walk.

    // --- live key editing ---------------------------------------------------
    // Both keyframe drags (the timeline bar and the easing graph) mutate the
    // block on every mouse-move and commit ONCE on release, so a drag is a
    // single Ctrl+Z rather than one per pixel. These calls are no-ops outside a
    // session, which is what keeps a stray edit from becoming un-undoable.
    bool liveEditActive(AdjustmentLayer *a) const;
    void liveSetKey(AdjustmentLayer *a, ParamId id, int frame, double value, Interp interp);
    void liveSetKeyValue(AdjustmentLayer *a, ParamId id, int frame, double value);
    void liveMoveKey(AdjustmentLayer *a, ParamId id, int from, int to);
    void liveSetHandles(AdjustmentLayer *a, ParamId id, int frame, double outInfl, double outSpeed,
                        double inInfl, double inSpeed);
    // Live camera key editing (same contract, backed by the camera record).
    bool liveCameraEditActive(CameraLayer *c) const;
    void liveMoveCameraKey(CameraLayer *c, ParamId id, int from, int to);
    void liveSetCameraKeyValue(CameraLayer *c, ParamId id, int frame, double value);
    void liveSetCameraKey(CameraLayer *c, ParamId id, int frame, double value, Interp interp);
    // Live write of an UN-ANIMATED camera parameter's static value (graph scrub).
    void liveSetCameraStaticParam(CameraLayer *c, ParamId id, double value);
    // Camera segment shaping for the easing graph (mirrors liveSetSegmentEase).
    bool liveSetCameraSegmentEase(CameraLayer *c, ParamId id, int frameA, double x1, double y1,
                                  double x2, double y2, int mirrorEnd = -1);
    void liveSetCameraSegmentInterp(CameraLayer *c, ParamId id, int frameA, Interp i);
    void previewCameraBar(CameraLayer *c, int keyDelta, const ParamBlock &base);
    void commitCameraBar(CameraLayer *c, const ParamBlock &base);
    void abortCameraBar(CameraLayer *c, const ParamBlock &base);

    // Shape ONE segment of one channel with normalised cubic-bezier control
    // points — the model the easing graph shows. x1/x2 are the usual 0..1
    // bezier abscissae (x1 from key A, x2 measured from key B's end); y1/y2 are
    // PROGRESS, so 0 is A's value and 1 is B's, and overshoot is allowed.
    //
    // This is the only place the normalised view is converted back into stored
    // handles (influence + speed), so the graph, the presets and every undo path
    // agree by construction:
    //   speed = y * dV / (influence * dF)
    // A segment with no value delta (dV == 0) has no progress to speak of; its
    // handles keep their stored height and only their TIME moves.
    // `mirrorEnd` (0 = key A, 1 = key B, -1 = none) also swings that key's OTHER
    // handle to the same slope, which is what keeps a shared key smooth across
    // the join; -1 leaves it alone (a broken tangent).
    bool liveSetSegmentEase(AdjustmentLayer *a, ParamId id, int frameA, double x1, double y1,
                            double x2, double y2, int mirrorEnd = -1);
    // The segment MODE written inside a live session, so a preset (mode + shape)
    // lands as ONE undo step rather than mode-then-shape. Only the left key's
    // interp decides a segment, so the neighbour's motion is untouched.
    void liveSetSegmentInterp(AdjustmentLayer *a, ParamId id, int frameA, Interp i);

    // Live write of an UN-ANIMATED parameter's static value, for the transform
    // panel's scrub. Outside a beginParamRecord session it is a no-op, so a stray
    // call can never produce an un-undoable edit.
    void liveSetStaticParam(AdjustmentLayer *a, ParamId id, double value);

    // --- per-cell layer stack (the right-side Layer panel's model) ---
    // The cell is the one exposed at the playhead on the current track: one
    // key plus its exposure span, owning its own layer stack. Nothing is
    // shared with any other cell.
    Cell *currentCell();          // nullptr when nothing is exposed there
    Cell *currentCellOrCreate();  // respects auto-key; nullptr when blocked
    int currentCellKey() const;   // cell start, or -1 when nothing is exposed
    // The paint target of that cell (Cell::activeId, else topmost paint layer).
    CellLayer *currentCellLayer();
    // Hit-test device for the WHOLE current cell: composites the cell's layer
    // stack (visible layers only; track chrome is not applied) into a reused
    // scratch. nullptr when nothing is exposed at the playhead; otherwise the
    // device always points at the scratch, whose extent() may be empty (a cell
    // with no visible content). The whole-stroke lasso scans this so ink on
    // ANY cell layer is grabbed, not just the active paint layer.
    const PaintDevice *compositeOfCurrentCell();
    void setCellLayerActive(CellLayer *l); // paint layers only
    // True when the track or the cell's active layer is locked (painting blocked).
    bool paintBlockedByLock();

    // Structure/property ops on the current cell. Every op invalidates the
    // frame cache for the cell and pushes an undo action, so Ctrl+Z restores
    // layers (and their pixels) exactly.
    // `above` = insert directly above that layer (same group); null = top.
    CellLayer *addCellLayer(const std::string &name = {}, CellLayer *above = nullptr);
    CellLayer *addCellGroup(const std::string &name = {}, CellLayer *above = nullptr);
    CellLayer *duplicateCellLayer(CellLayer *l);
    bool deleteCellLayer(CellLayer *l); // refuses the last paint layer
    bool moveCellLayer(CellLayer *l, int delta); // +1 = toward the front
    bool moveCellLayerToGroup(CellLayer *l, CellLayer *group); // null = top level
    // Drop a layer at an exact spot: `index` is the insertion position among
    // the target group's children in STACK order (0 = back-most, <0 = top),
    // as measured before the layer is removed. Used by panel drag & drop.
    bool moveCellLayerTo(CellLayer *l, CellLayer *group, int index);
    CellLayer *groupCellLayers(const std::vector<CellLayer*> &sel, const std::string &name = {});
    bool ungroupCellLayer(CellLayer *g);
    bool renameCellLayer(CellLayer *l, const std::string &name);
    bool setCellLayerVisible(CellLayer *l, bool v);
    bool setCellLayerLocked(CellLayer *l, bool v);
    bool setCellLayerOpacity(CellLayer *l, float o);
    bool setCellLayerBlend(CellLayer *l, BlendMode b);

private:
    // Where a cell layer sits: enough to put it back exactly (undo/redo).
    struct CellSlot {
        std::string trackId;
        int key{-1};
        uint64_t layerId{0};
        uint64_t parentId{0}; // 0 = top level
        int index{0};
    };
    struct CellRef {
        PaintLayer *track{nullptr};
        int key{-1};
        Cell *cell{nullptr};
    };
    CellRef currentCellRef(bool createIfMissing);
    void beginEdit(); // EditGuard plumbing (recursive, UI thread only)
    void endEdit();
    CellSlot slotOfCellLayer(PaintLayer *track, int key, Cell *cell, const CellLayer *l) const;
    // Undo/redo primitives: place a held layer at a slot (detaching it first
    // wherever it currently lives), or detach it.
    bool placeCellLayer(const CellSlot &slot, std::shared_ptr<CellLayer> layer);
    bool detachCellLayerAt(const CellSlot &slot);
    std::shared_ptr<CellLayer> cellLayerById(const CellSlot &slot) const;
    void touchCell(int key); // invalidate the frame cache for a cell's frames
    // Snapshot-based undo for parameter edits: `before` is the block as it was
    // before the caller mutated it. Returns false (and pushes nothing) when the
    // edit turned out to be a no-op. Accepts the compat view or a paint layer.
    bool pushParamEdit(const char *name, AdjustmentLayer *a, const ParamBlock &before);
    bool pushCameraEdit(const char *name, CameraLayer *c, const ParamBlock &before);
    // One recorded tool drag: the block as it was when the drag began, so the
    // whole drag is a single Ctrl+Z. The layer pointer is the compat VIEW, whose
    // params() aliases the real paint layer's block.
    struct ParamRecord {
        AdjustmentLayer *layer{nullptr};
        ParamBlock before;
        std::string label;
        int frame{0};
        bool active{false};
    };
    struct CameraRecord {
        CameraLayer *cam{nullptr};
        ParamBlock before;
        std::string label;
        int frame{0};
        bool active{false};
    };

    void syncTimelineFromImage();
    std::unique_ptr<Image> image_;
    UndoStack undo_;
    TimelineModel timeline_;
    std::unique_ptr<FrameCache> cache_;
    PlaybackEngine playback_;
    OnionSkinConfig onion_;
    Selection selection_;
    PaintLayer *currentLayer_{nullptr};
    // The single compatibility view over the current layer's keyframes.
    std::unique_ptr<AdjustmentLayer> view_;
    AdjustmentLayer *currentAdjustment_{nullptr};
    GroupLayer *currentFolder_{nullptr};
    KeyClipboard clipboard_;
    ParamRecord record_;
    CameraRecord camRecord_;
    PaintDevice cellScratch_; // reused by compositeOfCurrentCell()
    bool autoKey_{true};
    int editDepth_{0}; // EditGuard nesting on the UI thread
    std::shared_mutex *editMutex_{nullptr}; // image_'s treeMutex()
};

} // namespace ink
