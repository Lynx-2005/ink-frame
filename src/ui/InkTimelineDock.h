#pragma once
// Timeline dock: dense transport + cell toolbar, the TimelineWidget grid
// (sticky gutter, paper cells, layer multi-select) and a footer status strip.
// Layer/folder ops live in the grid's gutter header, where the layer list is.
#include <QDockWidget>

class QComboBox;
class QLabel;
class QScrollBar;
class QSlider;
class QSpinBox;
class QTimer;
class QToolButton;

namespace ink {
class Document;
class TimelineWidget;
class PaintLayer;
class GroupLayer;

class TimelineDock : public QDockWidget {
    Q_OBJECT
public:
    explicit TimelineDock(Document *doc, QWidget *parent = nullptr);
    TimelineWidget *grid() { return grid_; }
    void refresh();        // re-read model, repaint grid + chrome
    void refreshContent(); // content changed: also drop cached cel thumbnails
signals:
    void frameChanged(int frame);
    void keyChanged();
    void layersChanged();
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
    void onRename(const QString &id);
    void showTrackMenu(const QPoint &globalPos, const QString &id);
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
    // Grow the project so `frame` exists; no-op while it already does.
    void ensureFrameCount(int frame);
    static int pxToSlider(double px);
    static double sliderToPx(int v);
    Document *doc_;
    TimelineWidget *grid_{nullptr};
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
    QScrollBar *hBar_{nullptr};
    QScrollBar *vBar_{nullptr};
    QSlider *zoomSlider_{nullptr};
    QLabel *zoomLabel_{nullptr};
    QTimer *timer_{nullptr};
    bool autoKey_{true};
    bool follow_{true};
};

} // namespace ink
