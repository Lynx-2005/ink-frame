#pragma once
// ToolContext: the shared state every tool works against — document handle,
// foreground/background colors, brush settings, shape settings and the
// dirty-rect plumbing back into the canvas. One place owns it, so the
// toolbox, the options bar and the tools can never drift apart.
#include "core/InkSelection.h"
#include "core/InkTileStore.h"
#include "resources/InkResources.h"
#include <QColor>
#include <QObject>
#include <QString>

namespace ink {

class Document;
class AdjustmentLayer;
class CameraLayer;
enum class ParamId : int;
class PaintDevice;
struct ToolHost;

class ToolContext : public QObject {
    Q_OBJECT
public:
    explicit ToolContext(Document *doc, QObject *parent = nullptr);

    void setHost(ToolHost *host) { host_ = host; }
    ToolHost *host() const { return host_; }
    Document *doc() const { return doc_; }
    int frame() const;

    // Current layer/frame paint target; nullptr when the layer is locked,
    // missing or not paintable. When auto-key is OFF and no cell is exposed,
    // also nullptr (no dummy cell is created) — see autoKeyBlocked().
    PaintDevice *editableDevice() const;
    bool canEdit() const { return editableDevice() != nullptr; }
    // True when painting is blocked ONLY because auto-key is off and the
    // current frame exposes no cell (vs locked/missing layer).
    bool autoKeyBlocked() const;
    // Status text for a null editableDevice(): autokey hint vs locked hint.
    QString blockedHint() const;

    // --- selection (per document) ---
    Selection &selection();
    // nullptr when there is nothing selected: tools then skip per-pixel work.
    const Selection *selectionClip() const;
    // Every tool funnels its selection edits through here. It also re-derives
    // the ants' display matrix: a NEW mask is built in device space, and on a
    // keyframed layer the ink it clips DISPLACES at composite time, so without
    // this the freshly drawn lasso's outline sat beside the stroke instead of
    // around it (the "ants on their own track" report). Frame changes, undo and
    // key edits re-synced; changing the selection itself did not.
    void notifySelectionChanged();

    // --- colors: foreground / background (X swaps, D resets) ---
    const QColor &primary() const { return primary_; }
    const QColor &secondary() const { return secondary_; }
    void setPrimaryColor(const QColor &c);
    void setSecondaryColor(const QColor &c);
    void swapColors();
    void resetColors();

    // --- brush settings (paint tools) ---
    const BrushPreset &preset() const { return preset_; }
    const ResourceDb &resources() const { return db_; }
    // Swap tip/dynamics to a named preset; keeps the current size.
    bool setPresetId(const QString &id);
    double size() const { return double(preset_.tip.diameter); }
    void setSize(double px);
    void setOpacity(int percent);
    void setHardness(int percent);
    void setSpacing(int percent);
    void setPressureSize(bool on);
    void setPressureOpacity(bool on);
    void setSmoothing(int percent);

    // --- shape tools: own stroke width, never the brush size ---
    double shapeWidth() const { return shapeWidth_; }
    void setShapeWidth(double px);
    bool shapeFill() const { return shapeFill_; }
    void setShapeFill(bool on);

    // --- painting plumbing ---
    void markDirty(const Rect &r);
    Rect takeDirty();
    void requestRepaint();
    void requestRebuild();
    // Publish an undoable edit: memento (before) + after-tiles (redo),
    // then refresh the canvas projection and let the shell sync.
    void pushStrokeCommand(const QString &name, PaintDevice *device, TileMemento memento);
    void setStatus(const QString &text) { emit statusChanged(text); }

    // --- keyframe recording -------------------------------------------------
    // The adjustment layer linked to the working layer, or nullptr. This is the
    // seam that makes keyframes tool-agnostic: a tool never looks for keyframes,
    // it only asks "is anything recording what I am about to do?".
    AdjustmentLayer *recordTarget() const;
    // Auto-key armed AND something to record into.
    bool recording() const;
    // The value the record target already evaluates to at `frame`, so a drag
    // starts from the animated pose instead of snapping to zero.
    double recordedValue(ParamId id, int frame) const;
    // Write during a drag. Cheap on purpose: no undo entry is pushed until
    // endParamRecord(), so a whole drag is ONE Ctrl+Z.
    void recordParam(ParamId id, double value, int frame);
    void beginParamRecord(const QString &label, int frame);
    void endParamRecord();
    bool recordActive() const;
    // --- camera recording (same contract, camera undo stream) ---
    // The CameraTool resolves its own target (menus own creation); these just
    // forward to Document and then tell the shell to repaint the timeline's
    // diamonds immediately — the canvas rebuild alone never repaints them,
    // which is why camera keys used to appear only after wiggling the mouse.
    CameraLayer *cameraTarget() const;
    void beginCameraRecord(const QString &label, CameraLayer *cam);
    void recordCameraValue(CameraLayer *cam, ParamId id, double value, int frame);
    void endCameraRecord();
    void cancelCameraRecord();
    bool cameraRecordActive() const;
    // Mid-drag preview: the values are already written, this only refreshes
    // the timeline row (cheap update, no thumbnail invalidation).
    void notifyCameraPreview();

signals:
    void colorsChanged();
    void settingsChanged(); // brush tip/dynamics or shape options changed
    void selectionChanged();
    void strokeCommitted();
    void statusChanged(const QString &text);
    // A recorded drag changed the animation (canvas + timeline both react).
    void animationRecorded();
    // A camera drag wrote values mid-gesture: repaint the camera lane now.
    void cameraPreviewChanged();

private:
    Document *doc_;
    ToolHost *host_{nullptr};
    ResourceDb db_;
    BrushPreset preset_;
    QColor primary_{20, 22, 26};
    QColor secondary_{255, 255, 255};
    double shapeWidth_{8.0};
    bool shapeFill_{false};
    Rect pendingDirty_;
};

} // namespace ink
