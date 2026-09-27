#pragma once
// Canvas: QOpenGLWidget renderer with ONE shared view transform (the
// ViewConverter is the only source of truth for screen mapping). The canvas
// owns no tool state: pointer input is mapped into document space and handed
// to the ToolManager, and the active tool draws its own overlay through the
// paintOverlay hook while it is in document space.
#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QTabletEvent>
#include <QTimer>
#include <QDateTime>
#include <map>
#include <memory>
#include <vector>
#include "core/InkPaintDevice.h"
#include "core/InkTileStore.h"
#include "core/InkViewConverter.h"
#include "ui/tools/InkTool.h" // ToolHost + ToolEvent

namespace ink {

class Document;
class ToolContext;
class ToolManager;

class CanvasWidget : public QOpenGLWidget, protected QOpenGLFunctions, public ToolHost {
    Q_OBJECT
public:
    explicit CanvasWidget(Document *doc, QWidget *parent = nullptr);

    void setToolManager(ToolManager *tools);
    void setToolContext(ToolContext *ctx);

    // ToolHost: how tools request redraws without knowing this class.
    void repaint() override { update(); }
    void rebuild() override { markDirty(); }
    double nowSec() const override {
        return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    }

    void fitToView();
    void markDirty(); // projection/ghosts must be rebuilt
    double zoom() const { return view_.zoom(); }
    void setZoom(double z); // absolute zoom (top-bar dropdown)
    // View rotation (canvas only, never the pixels): 4/6 nudge it, 5 clears it,
    // the top bar picks absolute angles.
    double rotationDeg() const { return view_.rotationDeg(); }
    void setRotation(double deg);
    void rotateBy(double deg);
    void resetRotation() { setRotation(0.0); }
    void setActualSize(); // zoom to 100%

signals:
    void viewChanged();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void leaveEvent(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;

private:
    ToolEvent makeEvent(const QPointF &docPos, float pressure, double timeSec,
                        Qt::KeyboardModifiers mods, bool tablet) const;
    QPointF toCanvas(const QPointF &widgetPos) const;
    QTransform qtransform() const;
    void syncViewSizes();
    void updateCursor();
    void updateAnts(); // start/stop the marching-ants animation
    // Projection of frame f: frame cache first, synchronous fallback.
    // When live==true (edit in progress) the cache is bypassed for the
    // current frame: the cache holds the pre-stroke image, so using it would
    // hide dabs until release. Sync project sees the live device.
    std::shared_ptr<PaintDevice> frameImage(int frame, bool live);
    // Full rebuild (frame change, edits outside strokes, undo, layer ops):
    // re-projects everything, re-applies ghosts, re-uploads the image.
    void rebuildComposed(bool live);
    // Incremental path (the buttery-smooth stroke loop): re-projects ONLY the
    // accumulated dirty rect through the whole layer stack and patches just
    // that rect into the persistent image.
    void incrementalUpdate(const Rect &dirty);
    std::shared_ptr<PaintDevice> ghostImage(int frame);
    void applyGhosts(PaintDevice &composed, const Rect *clip);
    void uploadRect(const Rect &r);
    // The canvas checkerboard, rasterized once per document size instead of
    // 3600 fillRect calls per paintGL (which ran on every mouse move).
    void ensureChecker(int w, int h);

    Document *doc_;
    ToolManager *tools_{nullptr};
    ToolContext *ctx_{nullptr};
    ViewConverter view_; // THE transform (doc <-> widget, logical px)
    bool fitted_{false};
    // input state owned by the canvas: pan (space or middle drag) + hover
    bool spaceHeld_{false};
    bool panning_{false};
    QPoint panStart_;
    bool toolPressed_{false};
    bool hover_{false};
    QPointF hoverPos_;
    // marching ants: phase + timer, only running while a selection exists
    QTimer *antsTimer_{nullptr};
    int antsPhase_{0};

    // Composed view: persistent device + persistent full-canvas image.
    bool composedDirty_{true};
    int composedFrame_{-1};
    PaintDevice composedDev_;
    // Scratch for the incremental path: the dirty rect is re-projected here so
    // tiles can be rebuilt ghosts-first with the active frame over them.
    PaintDevice scratchDev_;
    QImage composedImage_; // full canvas size, patched incrementally
    QImage checker_;       // cached transparency checkerboard
    // Reused readback buffer for uploadRect (was: malloc+free per dab).
    std::vector<uint8_t> uploadBuf_;
    // Ghost-frame projections, valid for the current playhead position only.
    std::map<int, std::shared_ptr<PaintDevice>> ghostDevs_;
};

} // namespace ink
