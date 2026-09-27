#pragma once
// Move tool: the selection tools define the region, this is what drags it -
// the Krita/Photoshop split (lasso selects, move tool moves the pixels).
// Alt leaves a copy behind, Shift constrains to one axis, arrows nudge.
#include "core/InkRegion.h"
#include "core/InkSelection.h"
#include "ui/tools/InkTool.h"
#include <QPointer>

class QToolButton;

namespace ink {

class ToolContext;

class MoveTool : public Tool {
public:
    QString id() const override { return QStringLiteral("move"); }
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    bool nudge(int dx, int dy, Qt::KeyboardModifiers mods) override;
    void flipSelection(bool horizontal) override { flip(horizontal); }
    bool isEditing() const override { return dragging_; }
    QCursor cursor() const override { return QCursor(Qt::SizeAllCursor); }
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override;

    // Opt-in keyframe recording ("Record as keyframes" toggle). A setter, so
    // UI and programmatic changes settle an in-flight drag identically.
    bool recordKeyframes() const { return recordKeyframes_; }
    void setRecordKeyframes(bool on);

private:
    void resetState();
    bool grab(bool useSelection);
    bool grabAt(const Vec2 &at);
    Rect documentRect() const;
    // The animation's own map for this session (device -> display) and its
    // inverse. A keyed layer DISPLACES its pixels - it may rotate, scale, shear
    // or perspective-warp them - while this tool lifts, erases, stamps and
    // hit-tests in DEVICE space. Measuring the pointer in display space was
    // only ever right for a pure translation: on a rotated layer the mask
    // hit-test missed, and the offset written to the Position keys (and the
    // stamp) was the SCREEN delta, not the one the pixels moved by.
    void refreshDisplayMapping();
    Vec2 toDevice(const Vec2 &p) const { return displayToState_.map(p); }
    // A screen-space vector (an arrow-key nudge) as the device-space vector that
    // moves the ink the same way ON SCREEN.
    Vec2 toDeviceVector(const Vec2 &v) const {
        return {displayToState_.m[0] * v.x + displayToState_.m[1] * v.y,
                displayToState_.m[3] * v.x + displayToState_.m[4] * v.y};
    }
    const Selection *mask() const;
    void applyOffset(int dx, int dy);
    void flip(bool horizontal);
    QString commandName() const;

    // --- keyframe recording ---
    // With "Record as keyframes" ticked, Auto-key armed and a linked adjustment
    // layer, dragging the WHOLE layer records a Position keyframe instead of
    // moving pixels. A drag that started inside a selection keeps editing
    // pixels: a selection move is a local raster edit, and animating the layer
    // around it would move the rest of the drawing too.
    bool canRecord() const;
    bool beginRecord(const Vec2 &at);
    void recordOffset(double dx, double dy);
    // End the open record; when the drag planted a channel's first key, say
    // so (Add-key an earlier frame to animate from there). Never auto-keys.
    void closeRecordWithHint();

    // Recording is OPT-IN (default off): with Auto-key ON by default, the
    // takeover silently swallowed every layer drag as soon as a linked
    // adjustment existed — the "move tool broken" regression. The toggle rides
    // the move tool's options row like the transform tool's.
    bool recordKeyframes_{false};

    Mat3 stateToDisplay_{};
    Mat3 displayToState_{};
    ToolContext *ctx_{nullptr};
    PaintDevice *device_{nullptr};
    FloatRegion lifted_;
    Selection eraseMask_; // mask as it was when the drag started
    TileMemento memento_;
    Rect area_{};
    bool dragging_{false};
    bool copying_{false};
    bool liftedOnce_{false};
    bool selectionScope_{false};
    bool hadSelection_{false};
    bool recording_{false};
    int recordFrame_{-1};
    double baseX_{0}, baseY_{0}; // recorded position the drag started from
    // Channels keyless when this record started: keying one of them plants its
    // FIRST key at the playhead (holds backwards), so the close names them in
    // the status instead of auto-keying anything.
    bool freshX_{false}, freshY_{false};
    Vec2 start_{};
    int offsetX_{0}, offsetY_{0};
};

} // namespace ink
