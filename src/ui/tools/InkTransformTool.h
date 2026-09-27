#pragma once
// Transform tool family: Free (handles), Perspective (quad), Warp (MLS point
// deformation) and Liquify (brush) live in ONE tool class, the way Krita's
// tool_transform2 hosts its per-mode strategies.
//
// Session model — the load-bearing part: the pixels are lifted ONCE and every
// manipulation rebuilds the destination from that pristine lift (restore
// memento -> erase source -> stamp the transformed region), the same loop the
// move tool proved. N dragged frames therefore produce exactly the pixels a
// single Apply would, and the whole session is one undo memento.
#include "core/InkDeform.h"
#include "core/InkParams.h"
#include "core/InkRegion.h"
#include "core/InkSelection.h"
#include "core/InkTransform.h"
#include "ui/tools/InkTool.h"
#include <QPointer>
#include <QString>
#include <vector>

class QDoubleSpinBox;
class QLabel;
class QPolygonF;
class QStackedWidget;
class QToolButton;
class QWidget;

namespace ink {

class ToolContext;

class TransformTool : public Tool {
public:
    enum class Mode { Free, Perspective, Warp, Liquify };

    TransformTool(Mode initial, QString factoryId);

    QString id() const override { return factoryId_; }
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    bool commit() override;
    // Opt-in keyframe recording ("Record as keyframes" toggle). A setter so UI
    // and programmatic changes settle the session through one code path.
    bool recordKeyframes() const { return recordKeyframes_; }
    void setRecordKeyframes(bool on);
    // Switch between Free / Perspective / Warp / Liquify in place (the options
    // bar MODE combo): the pending session settles first, then a fresh one
    // begins in the newly chosen mode.
    void setMode(Mode m);
    void flipSelection(bool horizontal) override;
    bool isEditing() const override { return pending_; }
    // The pivot the next grab will rotate about, AS DRAWN (display space).
    // A test seam proving the dot sits on the displayed ink, not the stored
    // pixels — which is exactly what the record-mode seeding must guarantee.
    Vec2 pivotForTest() const { return pivotShown(); }
    // The display offset the session is applying, and the warp anchor hit test
    // at a DISPLAY position: together they prove the pointer is mapped into
    // device space before it reaches the mesh. `offsetForTest` is the
    // translation part only (kept for the older tests); the matrix seams are
    // what a rotation-aware test must assert on.
    Vec2 offsetForTest() const { return {stateToDisplay_.m[2], stateToDisplay_.m[5]}; }
    Mat3 displayMatrixForTest() const { return stateToDisplay_; }
    Vec2 toDeviceForTest(const Vec2 &p) const { return toDevice(p); }
    Vec2 toDisplayForTest(const Vec2 &p) const { return toDisplay(p); }
    int anchorAtForTest(const Vec2 &displayPos, double zoom) const {
        return warpAnchorAt(displayPos, zoom);
    }
    // The warp anchor grid as the session holds it (device space), and the
    // centre of the region being transformed.
    const std::vector<Vec2> &warpAnchorsForTest() const { return warp_.movedPts; }
    Vec2 liftCentreForTest() const {
        return {double(lift_.bounds.x) + lift_.bounds.w * 0.5,
                double(lift_.bounds.y) + lift_.bounds.h * 0.5};
    }
    // The frame's four corners as they are DRAWN (display space), so a test can
    // aim a corner grab at the real handle instead of re-deriving the geometry.
    void frameCornersForTest(Vec2 out[4]) const { frameCorners(out); }
    QCursor cursor() const override;
    QCursor cursorAt(const Vec2 &docPos, double zoom) const override;
    void paintOverlay(QPainter &p, const OverlayInfo &info) const override;
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override;
    // Release-time status: the drag is a preview until it is Applied.
    QString previewStatus() const;

private:
    // What the pointer grabbed at press time. "Outside" means "the frame
    // itself": the free transform rotates about the pivot from there.
    enum class Grip {
        None, Body, Outside, Pivot,
        CornerTL, CornerTR, CornerBR, CornerBL,
        EdgeTop, EdgeRight, EdgeBottom, EdgeLeft,
    };

    // Free transform parameters live in the engine (FreeTransformParams in
    // InkTransform.h) so the matrix composition is unit-tested headlessly.
    // Perspective mode keeps only its four destination corners; the source
    // stays the lifted rect, so the map is quad -> quad.
    struct PerspectiveState {
        Vec2 corners[4]{};

        void reset() {
            for (Vec2 &c : corners)
                c = {0.0, 0.0};
        }
    };

    // Warp mode: the anchor points the user drags plus the lattice they drive.
    // Anchors come either from a subdivision grid (density x density) or from
    // clicks ("Draw"); the lattice itself always stays fine.
    struct WarpState {
        std::vector<Vec2> origPts, movedPts;
        int dragIndex{-1};
        double alpha{1.0};  // flexibility: larger = more local
        int density{3};
        bool drawMode{false};
        MlsKind kind{MlsKind::Affine};

        void reset() {
            origPts.clear();
            movedPts.clear();
            dragIndex = -1;
        }
    };

    // Liquify: a brush that accumulates into a displacement field (the field
    // IS the state, like Krita's KisLiquifyTransformWorker grid).
    struct LiquifyState {
        LiquifyField field;
        LiquifyField::Brush brush{LiquifyField::Brush::Move};
        double size{60.0}; // brush diameter, document pixels
        double amount{0.25};
        double flow{0.5};
        double spacing{0.2}; // dab step, as a fraction of the diameter
        bool wash{false};
        bool stroking{false};
        Vec2 lastDab;
        Vec2 strokeRef; // press position: Scale/Rotate read the drag from it

        void resetState() {
            stroking = false;
            lastDab = {0.0, 0.0};
            strokeRef = {0.0, 0.0};
        }
    };

    // --- session ---
    bool beginSession();
    void resetSession();
    // Rebuild the lift + mode state from the CURRENT device/params WITHOUT
    // dropping the session: what commit() and cancelPrimary() settle into, so
    // the cage survives a release and always outlines the latest state. A
    // fresh beginSession() after a reset, minus the teardown.
    void reseedSession();
    // The playhead or the layer can move under a pending session (the timeline
    // is a separate widget): settle the old cel and start on the new one.
    void ensureSessionTarget();
    // True when the animation's pose at the session's own frame has changed
    // under us (a key added, moved, undone) — the frame must be re-seeded so it
    // never shows a pose the model no longer holds.
    bool recordedPoseChanged() const;
    Rect documentRect() const;
    double handleRadiusInDoc(double zoom) const;

    // --- keyframe recording ---
    // When Auto-key is armed and the working layer has a linked adjustment
    // layer, a drag drives that layer's parameters instead of baking pixels:
    // the pixels in the cel are never touched, and the whole drag is one undo
    // step. Nothing else in this tool changes — the handles, hit-testing and
    // overlay are the same code on both paths.
    bool canRecord() const;
    bool beginRecordSession();
    void pushRecordedParams(int frame);
    // The parameter values as they were when the drag began. Only what this drag
    // actually MOVES gets keyed: dragging a layer across the canvas must not
    // silently key its scale.
    struct RecordBase {
        double posX{0}, posY{0}, anchorX{0}, anchorY{0};
        double scaleX{1}, scaleY{1}, rot{0}, shearX{0}, shearY{0};
        Vec2 corners[4]{};
    };
    void snapshotRecordBase();
    RecordBase recBase_;
    bool recBaseValid_{false};
    // Writable params that held NO keys when this record session started. If
    // the drag keys one of them, its channel gains its first key at the
    // playhead — which holds backwards, so earlier frames will NOT move. The
    // commit hint names them (never auto-keys: the user asked for no
    // automatic keyframes).
    std::vector<ParamId> freshParams_;
    // Status text for a landed record, extended with the first-key hint.
    QString recordCommitStatus() const;
    // The first-key hint on its own (shared by the release preview and the
    // Apply status): which channels gained their first key this session.
    QString firstKeyHint() const;
    static ParamId cornerXParam(int i);
    static ParamId cornerYParam(int i);

    // --- geometry ---
    Mat3 affineMatrix() const;
    Mat3 currentMatrix() const;
    // Map a pointer position from DISPLAY space into the space THIS SESSION'S
    // STATE LIVES IN. That space is not always device space, which is the whole
    // subtlety:
    //   - pixel mode: the state is device space (the pixels are stored there),
    //     and keyed ink DISPLAYS through ParamBlock::matrixFor - it may
    //     ROTATE, scale, shear or perspective-warp, not merely slide. So this is
    //     the inverse of the animation's own matrix. Compensating only its
    //     translation (the old Vec2 offset) is what made Warp and Liquify grab
    //     and smear the wrong pixels, and put the box off the stroke.
    //   - record mode (Free and Perspective alike): the state is FRAME-relative
    //     (pivot, anchor, corners), so the mapping is the inverse of the
    //     recorded affine - the very pose the compositor applies on top.
    // Every mode routes its pointer through here, so this pair is the only place
    // the two spaces are allowed to meet.
    Vec2 toDevice(const Vec2 &p) const { return displayToState_.map(p); }
    // The other direction: how the session's state is DRAWN. Never use it for
    // drag math - that is what moved a perspective corner into the wrong space.
    Vec2 toDisplay(const Vec2 &p) const { return stateToDisplay_.map(p); }
    // The same map for whole regions (the Warp/Liquify outline, which is a
    // device-space rect the user must see around the displayed stroke).
    Mat3 stateToDisplay() const { return stateToDisplay_; }
    // True when the animation displaces the ink at all: the two modes that bake
    // pixels need it to know whether the pointer mapping is even non-trivial.
    bool displayDisplaced() const { return !displayToState_.isIdentity(); }
    // (Re)derive both directions from the current session. Pixel mode seeds them
    // from the animation; record mode from the recorded pose (identity for Free,
    // the affine part for Perspective). Called wherever the pose may have moved.
    void refreshDisplayMapping();
    // The rect the quad override / perspective map is built against. In pixel
    // mode that is the lifted content bbox. In record mode it is the block's
    // reference FRAME (the rect the anchor and corners normalize against):
    // the frame the keys were authored from, which is the lift for a fresh
    // block and a stable rect afterwards — the one source rect that makes the
    // live handles, the keyed values and the compositor agree.
    Rect transformSourceRect() const;
    Rect liftBounds() const { return lift_.bounds; }
    // The pivot as DRAWN and hit-tested (display space): the state pivot run
    // through the session matrix, plus the animation offset in pixel mode
    // (whose matrix is device-space). The dot must travel with the frame —
    // drawing the raw pivot leaves it behind on every move drag and, in
    // record mode, double-shifts it away from the displayed stroke.
    Vec2 pivotShown() const;
    // The affine part of the RECORDED pose at the session frame (position,
    // anchor, scale, rotation, shear — never the quad): what the compositor
    // applies on top of a perspective quad, so the record cage can be drawn
    // where the stroke displays while the corners stay authored in frame
    // space. Identity when nothing is recorded.
    Mat3 recordedAffine() const;
    void frameCorners(Vec2 out[4]) const;
    void frameHandles(Vec2 out[8]) const;
    // A device-space rect as the polygon the user SEES, for the Warp/Liquify
    // region outline. Display-space geometry must go through the same map as
    // everything else - an axis-aligned rect plus an offset cannot describe a
    // rotated layer, which is what made those outlines miss the stroke.
    QPolygonF mappedQuad(const Rect &r) const;
    // `shear` (optional) reports an edge grab that missed the midpoint.
    Grip hitTest(const Vec2 &p, double zoom, bool *shear = nullptr) const;

    // --- warp ---
    void buildWarpAnchors();
    void rebuildWarpMesh();
    int warpAnchorAt(const Vec2 &p, double zoom) const;

    // --- interaction ---
    void dragFree(const ToolEvent &e);
    void dragPerspective(const ToolEvent &e);
    void dragWarp(const ToolEvent &e);
    void liquifyDab(const Vec2 &at);
    bool liquifySizeAt(const Vec2 &docPos) const;
    int cornerIndexOf(Grip g) const;
    // The field a mode deforms with, or nullptr for the matrix modes.
    const DeformMesh *activeMesh() const;

    // --- preview + commit ---
    void revertPending();
    void renderPreview(bool fast);
    // Is the expensive preview due? A drag re-resamples the region (or
    // re-projects the whole canvas in record mode) on every mouse-move, and at
    // 100+ events/second that starves the UI thread - the "hangs and lags" half
    // of the report. Every DRAG path goes through this; a slider, a flip or a
    // mode switch renders immediately, because those are one-shot and already
    // cost the user nothing.
    //
    // A host with no clock (ToolHost::nowSec() == 0, its documented default)
    // never throttles, which is what the headless tests rely on.
    bool previewDue();
    // Reset ONLY the active mode's parameters to their defaults (other modes
    // are untouched). Used by the per-mode Reset button and Esc.
    void resetModeParams();
    void syncOptions();
    void rotateQuarter(bool clockwise);
    QString commandName() const;

    Mode mode_;
    QString factoryId_;
    FreeTransformParams free_;
    bool keepAspect_{false}; // aspect link, a tool preference
    PerspectiveState persp_;
    WarpState warp_;
    DeformMesh warpMesh_;
    LiquifyState liquify_;

    PaintDevice *device_{nullptr};
    FloatRegion lift_;
    Selection eraseMask_; // the selection as it was when the lift happened
    // State space -> display space for this session, and its inverse. Both are
    // set together in one place (refreshDisplayMapping) from whatever the mode's
    // state means: the animation's own matrix in pixel mode, the recorded affine
    // in record mode. The pointer comes in through the inverse; the overlay and
    // every hit-test go out through the forward. They must never disagree, so
    // they are cached as a pair.
    Mat3 stateToDisplay_{};
    Mat3 displayToState_{};
    // The map this session was SEEDED with. A pixel session's whole frame, every
    // grab and every overlay are expressed through it, so if the animation moves
    // under an open session (a key dragged in the timeline, the panel scrubbing
    // a rotation) the mapping is stale and the box walks off the stroke again.
    // ensureSessionTarget compares against this and re-seeds.
    Mat3 seededDisplay_{};
    // The block's reference frame this record session normalizes against
    // (set in beginRecordSession, cleared in resetSession). transformSourceRect
    // reads it so the handles, the keyed values and the compositor share one
    // rect.
    Rect recordFrame_{};
    TileMemento memento_;
    Rect area_{}; // document rect the lift came from
    Rect lastStamp_{}; // device rect the previous preview frame stamped: the
                       // next frame must repaint it too, or the incremental
                       // canvas patch leaves a ghost of the old position.
    int sessionFrame_{-1};
    bool selectionScope_{false};
    bool session_{false};
    bool pending_{false}; // uncommitted pixels sit on the device right now
    bool dragging_{false};
    bool shearGrab_{false};
    bool recordMode_{false}; // driving keyframes, not pixels
    // Recording is OPT-IN: auto-key alone must not silently turn every
    // transform into a keyframe edit — that takeover is what broke pixel
    // transforms after the adjustment-layer refactor. The ONE toggle lives in
    // the tool's options-bar row (matrix modes only); the settings popup holds
    // numbers, not mode switches.
    bool recordKeyframes_{false};
    QPointer<QToolButton> recordToggle_;

    Grip grip_{Grip::None};
    Vec2 dragStart_{};
    Vec2 dragHandleStart_{}; // grabbed handle position when the drag began
    double scaleXStart_{1.0}, scaleYStart_{1.0}, angleStart_{0.0}, shearXStart_{0.0},
        shearYStart_{0.0};
    Vec2 translateStart_{};
    Vec2 pivotStart_{};
    Vec2 perspStart_[4]{};

    Interpolation filter_{Interpolation::Bicubic};
    bool fastPreview_{false};
    // Drag throttle (~30fps) shared by every mode, plus one queued flag so the
    // LAST mouse-move of a burst always lands - on release, at full quality.
    double lastPreviewSec_{-1.0};
    bool previewQueued_{false};

    // Options controls, mirrored so a drag updates the fields live.
    QPointer<QDoubleSpinBox> wBox_, hBox_, angleBox_, shearXBox_, shearYBox_;
    // Collapsible settings popup: one page per mode (stacked, so only the
    // active mode's controls ever appear) plus a per-mode Reset. The popup
    // floats, so the slim bar can never widen the app.
    QPointer<QStackedWidget> settingsStack_{nullptr};
    QPointer<QLabel> settingsTitle_{nullptr};
};

// One-shot region transforms for the shell menus: flip or turn the selection
// contents (else the layer's content extent) in a single undo step. Mirrors the
// shell helpers in InkSelectionTools.h; unlike the tool these need no session.
enum class RegionOp { FlipHorizontal, FlipVertical, RotateCW, RotateCCW };
bool applyRegionOp(ToolContext &ctx, RegionOp op);

} // namespace ink
