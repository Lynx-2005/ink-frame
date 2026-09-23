#pragma once
// Canvas: QOpenGLWidget renderer with ONE shared view transform.
// All coordinate mapping (paint + events) goes through ViewConverter —
// the single source of truth (Krita KisCoordinatesConverter concept,
// clean-room implementation). Logical pixels everywhere; no DPR math
// outside the converter.
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QTabletEvent>
#include <QImage>
#include <map>
#include <memory>
#include "brush/InkPaintOp.h"
#include "core/InkTileStore.h"
#include "core/InkViewConverter.h"
#include "resources/InkResources.h"
#include "ui/InkToolRail.h" // ToolId (no cycle: rail never includes canvas)

namespace ink {
class Document;

enum class ShapeMode { Line, Rect, Ellipse };

class CanvasWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit CanvasWidget(Document *doc, QWidget *parent = nullptr);
    void setBrushPreset(const BrushPreset &p); // copies (owns its preset)
    void setEraser(bool e) { eraser_ = e; }
    void setBrushColor(const QColor &c);
    void setToolMode(ToolId t);
    void setBrushSize(double px); // rebuilds tip, keeps preset
    double brushSize() const { return brushSize_; }
    void setShapeMode(ShapeMode m) { shapeMode_ = m; }
    ShapeMode shapeMode() const { return shapeMode_; }
    void fitToView();
    void markDirty(); // projection/ghosts must be rebuilt
    double zoom() const { return view_.zoom(); }
    void setZoom(double z); // absolute zoom (top-bar dropdown)

signals:
    void strokeFinished();
    void viewChanged();
    void colorPicked(const QColor &c);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void beginStrokeAt(const QPointF &canvasPos, float pressure, double timeSec);
    void moveStrokeTo(const QPointF &canvasPos, float pressure, double timeSec);
    void endStroke();
    void commitShape(); // rasterize shape preview as one undoable op
    void pickAt(const QPointF &canvasPos);
    QPointF toCanvas(const QPointF &widgetPos) const;
    QTransform qtransform() const;
    void syncViewSizes();
    // Projection of frame f: frame cache first, synchronous fallback.
    // When live==true (stroke in progress) the cache is bypassed for the
    // current frame: the cache holds the pre-stroke image, so using it
    // would hide dabs until release. Sync project sees the live device.
    std::shared_ptr<PaintDevice> frameImage(int frame, bool live);
    // Full rebuild (frame change, edits outside strokes, undo, layer ops):
    // re-projects everything, re-applies ghosts, re-uploads the image.
    void rebuildComposed();
    // Incremental path (the buttery-smooth stroke loop): re-projects ONLY
    // the accumulated stroke dirty rect through the whole layer stack
    // (tile sparsity keeps hundreds of layers cheap) and patches just that
    // rect into the persistent image. No full projection, no full upload.
    void incrementalUpdate();
    void applyGhosts(PaintDevice &composed, const Rect *clip);
    void uploadRect(const Rect &r);

    Document *doc_;
    ViewConverter view_; // THE transform (doc <-> widget, logical px)
    bool fitted_{false};
    bool stroking_{false};
    bool eraser_{false};
    std::unique_ptr<PaintOp> op_;
    BrushPreset preset_; // owned by value — never a dangling pointer
    bool hasPreset_{false};
    TileMemento strokeMemento_;
    PaintDevice *strokeDevice_{nullptr};
    QPoint panStart_;
    // tool state: shape preview + picker + cursor ring
    ToolId toolMode_{ToolId::Brush};
    ShapeMode shapeMode_{ShapeMode::Line};
    double brushSize_{8.0};
    bool shaping_{false};
    QPointF shapeStart_;
    QPointF shapeCur_;
    bool hover_{false};
    QPointF hoverPos_;

    // Composed view: persistent device + persistent full-canvas image.
    // Full rebuilds happen on frame change / external edits; strokes patch
    // only their dirty rect (see incrementalUpdate).
    bool composedDirty_{true};
    int composedFrame_{-1};
    PaintDevice composedDev_;
    // Scratch for the incremental path: the dirty rect is re-projected here so
    // the tile can be rebuilt ghosts-first and the active frame blitted over
    // them (onion ghosts always sit UNDER the drawing being worked on).
    PaintDevice scratchDev_;
    QImage composedImage_; // full canvas size, patched incrementally
    Rect strokeDirty_;     // dabs since last paint (union via ctx.dirtyAccum)
    // Ghost-frame projections, valid for the current playhead position only.
    std::map<int, std::shared_ptr<PaintDevice>> ghostDevs_;
    std::shared_ptr<PaintDevice> ghostImage(int frame);
};

} // namespace ink
