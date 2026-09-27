#pragma once
// Timeline dock: dense transport + cell toolbar, the TimelineWidget grid
// (sticky gutter, paper cells, layer multi-select) and a footer status strip.
// Layer/folder ops live in the grid's gutter header, where the layer list is.
#include <QDockWidget>

#include <QPoint>
#include <QString>

class QComboBox;
class QLabel;
class QMenu;
class QScrollBar;
class QSlider;
class QSpinBox;
class QSplitter;
class QTimer;
class QToolButton;

namespace ink {
class AdjustmentLayer;
class CameraLayer;
class CurveEditor;
class Document;
class PaintLayer;
class GroupLayer;
class TimelineWidget;

class TimelineDock : public QDockWidget {
    Q_OBJECT
public:
    explicit TimelineDock(Document *doc, QWidget *parent = nullptr);
    TimelineWidget *grid() { return grid_; }
    CurveEditor *curve() { return curve_; }
    QToolButton *graphButton() { return graphBtn_; }
    void refresh();        // re-read model, repaint grid + chrome
    void refreshContent(); // content changed: also drop cached cel thumbnails
    // Stroke-commit path: drop ONE cell's thumbnail, keep the rest cached.
    // Full refreshContent() re-rendered every filmstrip cell synchronously on
    // release (cellComposite + Smooth scale per cell) — visible jank.
    void refreshPixels(const QString &trackId, int key);
    // Build the keyframe menu WITHOUT showing it, so a test can assert on the
    // entries. onKeyMenu is exactly this plus the modal exec(). Caller owns it.
    QMenu *buildKeyMenu(const QString &adjId, int frame);

signals:
    void frameChanged(int frame);
    void keyChanged();
    void layersChanged();
    // The working track changed (gutter click / new track): the Layer panel
    // shows another cell's stack and must re-read the model.
    void currentLayerChanged();
    // Onion skin settings changed: the canvas must rebuild its ghosts (they
    // are cached per playhead position, so nothing else would repaint them).
    void onionChanged();
private slots:
    void onPlayToggle();
    void onSkipStart();
    void onSkipEnd();
    void onPrevCell();
    void onNextCell();
    void onNewCell();
    void onDuplicateCell();
    void onDeleteCell();
    void onAutoKeyToggled(bool on);
    void onFollowToggled(bool on);
    void onOnionToggled(bool on);
    void showOnionMenu(const QPoint &pos);
    void onFpsChanged(int idx);
    void onLoopCycle();
    void onFrameRegisterChanged(int f);
    void onRangeChanged();
    void onLengthChanged(int frames);
    void onZoomSlider(int v);
    void onFitView();
    void onGridScroll(int hx, int vy);
    void onGridZoom(double px);
    void onGridRange(int in, int out);
    void onAddLayer();
    void onAddGroup();
    void onGroupSelected();
    void onUngroup();
    void onAddKey();
    void onRemoveKey();
    void onPrevKey();
    void onNextKey();
    void onGraphToggled(bool on);
    void onKeyMenu(const QPoint &globalPos, const QString &adjId, int frame);
    void onRename(const QString &id);
    void showTrackMenu(const QPoint &globalPos, const QString &id);
    void showCameraMenu(const QPoint &globalPos, CameraLayer *cam);
    void showLaneMenu(const QPoint &globalPos, const QString &trackId, int frame);
    void showRulerMenu(const QPoint &globalPos);
private:
    void tick(); // QTimer 16ms drives PlaybackEngine::advance
    void updateTimecode();
    void updateStatus();
    void syncBars();   // scrollbar ranges <-> grid metrics
    void syncZoom();
    void syncTransport();
    void syncFrameCount(); // length -> spin ranges, grid range, bars, status
    // Enable/disable and describe the keyframe buttons for the working layer.
    void syncKeyButtons();
    // Point the easing graph at the working row: the selected camera when one
    // is selected, else the working paint layer. Called when the graph opens
    // AND when the selection changes while it is open, so picking a camera
    // row swaps the panel to the camera graph instead of leaving the regular
    // transform graph on screen.
    void syncGraphTarget();
    // The working layer for keyframe ops: the paint layer itself (the
    // keyframes live on it), through its compatibility view.
    AdjustmentLayer *keyLayer() const;
    // The working camera: the first selected camera row, if any. Camera rows
    // win over paint layers for the K / prev / next / graph buttons, so the
    // buttons always act on the row the artist just clicked.
    CameraLayer *keyCamera() const;
    void applyPreset(int interp, bool easeIn, bool easeOut);
    // Grow the project so `frame` exists; no-op while it already does.
    void ensureFrameCount(int frame);
    static int pxToSlider(double px);
    static double sliderToPx(int v);
    Document *doc_;
    TimelineWidget *grid_{nullptr};
    CurveEditor *curve_{nullptr};
    int lastFollowFrame_{-1}; // tick()'s playhead-change poll (see TimelineDock::tick)
    QSplitter *split_{nullptr};
    QLabel *timeLabel_{nullptr};
    QLabel *totalLabel_{nullptr};
    QLabel *statusLabel_{nullptr};
    QSpinBox *frameSpin_{nullptr};
    QSpinBox *startSpin_{nullptr};
    QSpinBox *endSpin_{nullptr};
    QSpinBox *lengthSpin_{nullptr};
    QComboBox *fpsBox_{nullptr};
    QToolButton *playBtn_{nullptr};
    QToolButton *onionBtn_{nullptr};
    QToolButton *followBtn_{nullptr};
    QToolButton *autoKeyBtn_{nullptr};
    QToolButton *loopBtn_{nullptr};
    QToolButton *addKeyBtn_{nullptr};
    QToolButton *delKeyBtn_{nullptr};
    QToolButton *prevKeyBtn_{nullptr};
    QToolButton *nextKeyBtn_{nullptr};
    QToolButton *graphBtn_{nullptr};
    QScrollBar *hBar_{nullptr};
    QScrollBar *vBar_{nullptr};
    QSlider *zoomSlider_{nullptr};
    QLabel *zoomLabel_{nullptr};
    QTimer *timer_{nullptr};
    bool autoKey_{true};
    bool follow_{true};
};

} // namespace ink
