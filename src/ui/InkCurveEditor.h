#pragma once
// The easing graph: ONE parameter, ONE segment between two of its keys, drawn as
// a normalised cubic Bezier from bottom-left to top-right.
//
// Why this shape and not a value graph. A continuous multi-channel graph asks the
// user to read slopes across a timeline and grab 6px handles at some frame
// position — accurate, but not something you can aim at. What an animator
// actually asks is smaller: "this move, how does it accelerate?" That is a single
// 0..1 curve, and it is the same curve whether the move takes 3 frames or 300. So
// the graph is normalised on both axes and holds exactly two handles.
//
// Normalised is a VIEW, not a second model. The two control points convert to and
// from the stored handle units (influence + speed) on every drag, so this editor
// changes the same numbers the compositor evaluates:
//
//   segment A -> B, dF = fB - fA, dV = vB - vA
//   x1 = outInfl_A                y1 = outSpeed_A * outInfl_A * dF / dV
//   x2 = 1 - inInfl_B             y2 = 1 - inSpeed_B  * inInfl_B  * dF / dV
//
// (x, y) is the usual bezier parameter, so linear is the diagonal and "ease" is a
// handle pulled towards a corner. A FLAT segment (dV == 0) has no ratio to show:
// the handles still move in time, but their height is meaningless, so the panel
// says so and drags horizontally only, instead of inventing a shape.
#include <QPainter>
#include <QPoint>
#include <QSizePolicy>
#include <QString>
#include <QVector>
#include <QWidget>

#include "animation/InkKeyframe.h"
#include "core/InkLayer.h"
#include "core/InkParams.h"
#include <string>

class QComboBox;
class QLabel;

namespace ink {

class Document;
class AdjustmentLayer;
class ParamField;

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(Document *doc, QWidget *parent = nullptr);

    // The layer whose parameters are on offer. Changing it re-picks the channel
    // and segment, so the panel can never show a stale curve from another layer.
    void setAdjustment(AdjustmentLayer *a);
    // A camera layer instead of a paint layer: the same normalised curve, the
    // same value strip, writing through the camera undo stream. Cleared by
    // setAdjustment and vice versa — the panel shows one target at a time.
    void setCamera(CameraLayer *c);
    // Resolved live by stable id on every call: the tree owns the camera, so
    // a stored pointer would dangle after Delete camera (unlike the paint
    // target_, which the dock re-points through setAdjustment).
    CameraLayer *camera() const;
    // True while the panel shows a camera's curves instead of a paint layer's.
    // The panel is one widget with two targets — this is what paints the
    // "Camera Easing" header, camera tooltips and camera status line.
    bool isCameraMode() const { return camera() != nullptr; }
    // Human name of the current target ("Layer 1", "whole shot", paint name).
    QString targetTitle() const;
    // The view the editor draws, RESOLVED through the document every time.
    // Document keeps exactly one adjustment view alive and destroys the previous
    // one when the keyed layer changes (ensureAdjustmentFor recycles it), so a
    // cached AdjustmentLayer* is a use-after-free waiting for the artist to pick
    // another layer. The keys live on the PAINT LAYER, which the tree owns for
    // as long as the layer exists, so remembering that is stable and the view can
    // always be re-fetched.
    AdjustmentLayer *adjustment() const;
    void refresh(); // re-read the model and repaint
    // Re-locate the segment for the CURRENT playhead without rebuilding the
    // channel list: what the timeline calls on every frame change, including
    // playback. The segment follows the playhead by design, so this must be
    // cheap enough to run per tick.
    void followPlayhead();

    // --- state, and the test seams a headless run needs to aim at -----------
    bool hasSegment() const { return segValid_; }
    ParamId channel() const { return chan_; }
    // The dropdown shows the five TRANSFORM groups (Anchor, Position, Scale,
    // Rotation, Opacity); selecting one opens its curve and its value fields.
    int groupCount() const;
    int currentGroup() const;
    QString groupLabel(int i) const;
    void selectGroup(int i);
    QRect dropdownRect() const;
    // The value strip below the header: fields for the selected group only.
    ParamField *valueField(ParamId id) const;
    int segmentIndex() const { return segIndex_; }
    int segmentCount() const { return segCount_; }
    int frameA() const { return fA_; }
    int frameB() const { return fB_; }
    double valueA() const { return vA_; }
    double valueB() const { return vB_; }
    bool flat() const { return flat_; }
    bool holds() const { return mode_ == Interp::Hold; }
    // The RECONCILED mode of the segment. Linear and Hold segments ignore the
    // stored handles when evaluating, so the graph must know: drawing handles that
    // change nothing is the failure mode this whole panel is built to avoid.
    Interp mode() const { return mode_; }

    // The two normalised control points, x in 0..1, y unbounded (overshoot).
    QPointF normA() const { return npA_; }
    QPointF normB() const { return npB_; }

    QPointF handlePointA() const;
    QPointF handlePointB() const;
    QRect handleRectA() const;
    QRect handleRectB() const;
    QRect presetCell(int i) const;
    static int presetCount();
    QRect prevSegmentRect() const;
    QRect nextSegmentRect() const;
    QRect resetRect() const;
    int presetHighlight() const; // which thumbnail matches the stored handles
    // The header line the graph paints ("Scale · frames 0–12 · 100% → 160%").
    // Public so a test can assert it spells values exactly like the transform
    // panel's fields do; paintEvent draws this same string.
    QString statusText() const;

signals:
    void graphChanged(); // the shape changed: the timeline row must repaint
    void statusMessage(const QString &text);
    void paramsEdited(); // a value field changed a parameter

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    enum class Grab { None, HandleA, HandleB };
    enum class Which { A, B };

    struct ChanEntry {
        ParamId id{ParamId::Count};
        QString label;
    };

    void rebuildChannels();
    bool groupHasSegment(int g) const;
    // Resolve the segment of `chan_` that the playhead sits inside (or the last
    // one, when the playhead is past it). False if the channel cannot be eased.
    bool locateSegment();
    // Build/destroy the value fields for the selected group and place them in
    // the strip below the header.
    void rebuildValueFields();
    void layoutValueFields();
    void onValueScrub(ParamId id, double v);
    void onValueCommit(ParamId id, double v);
    // The parameter block on offer right now: the camera's, else the paint
    // layer's through its view, else nullptr. Every read goes through here so
    // cameras and paint layers share one graph implementation.
    const ParamBlock *block() const;
    void stepSegment(int dir);
    // Restore THIS segment's ease to a plain linear ramp (one undo step).
    void resetEase();
    // Normalised control point -> stored handle units, one undo-safe live write.
    void writeHandle(Which which, const QPointF &np, bool unified);

    // --- geometry ----------------------------------------------------------
    QRect plot() const;                      // the square 0..1 box
    QPointF toScreen(const QPointF &np) const; // y flips: progress goes up
    QPointF toNorm(const QPoint &px) const;
    int topBand() const;                     // header + channel chips
    void hover(const QPoint &pos);
    void drawPresetColumn(QPainter &p, bool enabled);
    void drawStatus(QPainter &p);

    // Set by setAdjustment: the layer the artist picked. Stable for the life of
    // the layer; the adjustment view is not.
    PaintLayer *target_{nullptr};
    // Set by setCamera: the camera's stable id ("L<id>"). Resolved on every
    // use, so deleting the camera empties the graph instead of dangling.
    std::string camId_;
    Document *doc_;
    QVector<ChanEntry> chanList_;
    ParamId chan_{ParamId::Count};
    int group_{0}; // dropdown selection: index into transformGroups()
    // The transform dropdown (child widget, sits in the header's empty space
    // between the segment counter and Reset).
    QComboBox *combo_{nullptr};
    // Value fields for the selected group, laid out under the header.
    QVector<ParamField *> valueFields_;
    QVector<ParamId> valueIds_;
    QVector<QLabel *> valueLabels_;
    bool scrubbingValue_{false};

    bool segValid_{false};
    int segIndex_{-1}, segCount_{0};
    int fA_{0}, fB_{0};
    double vA_{0.0}, vB_{0.0};
    bool flat_{false};
    Interp mode_{Interp::Linear};
    ScalarKey keyA_, keyB_;
    QPointF npA_{1.0 / 3.0, 1.0 / 3.0}, npB_{2.0 / 3.0, 2.0 / 3.0};

    Grab grab_{Grab::None};
    bool dragMoved_{false};
    int hoverArrow_{0}; // -1 previous segment, +1 next, 0 none
    bool hoverReset_{false};
};

} // namespace ink
