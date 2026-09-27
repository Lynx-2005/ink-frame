#include "ui/InkMainWindow.h"
#include "io/InkProject.h"
#include "ui/InkCanvasWidget.h"
#include "ui/InkDocument.h"
#include "ui/InkLayerPanel.h"
#include "ui/InkTheme.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkTimelineWidget.h"
#include "ui/InkToolBox.h"
#include "ui/InkToolOptionsBar.h"
#include "ui/InkTopBar.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkToolManager.h"
#include "ui/tools/InkTransformTool.h"
#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QProcess>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ink {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setStyleSheet(InkTheme::styleSheet());
    setWindowTitle(QStringLiteral("InkFrame"));
    doc_ = new Document(1920, 1080, 12); // concept art runs 12 fps

    // --- tool system: context owns settings, manager owns the active tool ---
    toolCtx_ = new ToolContext(doc_, this);
    tools_ = new ToolManager(toolCtx_, this);
    canvas_ = new CanvasWidget(doc_, this);
    canvas_->setToolContext(toolCtx_);
    canvas_->setToolManager(tools_);
    for (QAction *action : tools_->windowActions())
        addAction(action); // shortcuts stay live while the canvas has focus

    // --- top bar ---
    topBar_ = new TopBar(this);
    connect(topBar_, &TopBar::undoRequested, this, &MainWindow::onUndo);
    connect(topBar_, &TopBar::redoRequested, this, &MainWindow::onRedo);
    connect(topBar_, &TopBar::zoomPicked, this, &MainWindow::onZoomPicked);
    connect(topBar_, &TopBar::rotatePicked, this, &MainWindow::onRotatePicked);

    // --- toolbox | canvas card | layer panel ---
    toolBox_ = new ToolBox(tools_, toolCtx_, this);
    auto *center = new QWidget(this);
    auto *hl = new QHBoxLayout(center);
    hl->setContentsMargins(8, 8, 8, 8);
    hl->setSpacing(8);
    hl->addWidget(toolBox_);
    auto *canvasCard = new QWidget(center);
    canvasCard->setObjectName(QStringLiteral("Card"));
    auto *cl = new QVBoxLayout(canvasCard);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    toolOpts_ = new ToolOptionsBar(tools_, canvasCard);
    cl->addWidget(toolOpts_);
    cl->addWidget(canvas_, 1);
    hl->addWidget(canvasCard, 1);

    // The Layer panel edits the layer stack of the cell under the playhead on
    // the current track. It is a plain widget (not a dock) exactly like the
    // toolbox, so canvas focus behaviour stays unchanged.
    layerPanel_ = new LayerPanel(doc_, center);
    hl->addWidget(layerPanel_);

    // Wrap center in a plain widget (not a dock) so canvas keeps focus.
    auto *mid = new QWidget(this);
    auto *ml = new QVBoxLayout(mid);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    ml->addWidget(topBar_);
    ml->addWidget(center, 1);
    setCentralWidget(mid);

    timeline_ = new TimelineDock(doc_, this);
    addDockWidget(Qt::BottomDockWidgetArea, timeline_);
    timeline_->setMinimumHeight(260);

    // --- layer panel toggle: top-bar three-dot button + F7, one state ---
    layersAction_ = new QAction(QStringLiteral("Layer panel"), this);
    layersAction_->setCheckable(true);
    layersAction_->setShortcut(QKeySequence(QStringLiteral("F7")));
    addAction(layersAction_);
    // The panel's visibility is the single source of truth and the two controls
    // only MIRROR it. setChecked() on a checkable QAction re-emits `toggled`,
    // so mirroring into it unblocked re-entered this setter; the button and the
    // action then drifted off the panel's real visibility, which is what made
    // the toggle take two or three presses to take effect. Block the signal and
    // skip no-op writes, so syncPanelControls is idempotent and safe to call
    // from either control.
    layerPanelVisible_ = true;
    auto syncPanelControls = [this](bool on) {
        layerPanelVisible_ = on;
        layerPanel_->setVisible(on);
        topBar_->setLayersChecked(on);
        if (layersAction_->isChecked() != on) {
            const QSignalBlocker block(layersAction_);
            layersAction_->setChecked(on);
        }
    };
    connect(topBar_, &TopBar::layersToggled, this, syncPanelControls);
    connect(layersAction_, &QAction::toggled, this,
            [this, syncPanelControls] { syncPanelControls(!layerPanelVisible_); });
    // The panel is on screen at startup, so the controls must start agreeing
    // with it — otherwise the button reads unchecked beside a plainly visible
    // panel, and the first press appears to do nothing.
    syncPanelControls(layerPanelVisible_);

    // Undo/redo shortcuts used to come from the Edit menu; they are window
    // actions now (the top bar's buttons are the visible entry points).
    auto *undoAction = new QAction(QStringLiteral("Undo"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    addAction(undoAction);
    connect(undoAction, &QAction::triggered, this, &MainWindow::onUndo);
    auto *redoAction = new QAction(QStringLiteral("Redo"), this);
    redoAction->setShortcut(QKeySequence::Redo);
    addAction(redoAction);
    connect(redoAction, &QAction::triggered, this, &MainWindow::onRedo);

    connect(timeline_, &TimelineDock::frameChanged, this, [this](int) {
        canvas_->markDirty();
        layerPanel_->refresh(); // another cell = another layer stack
    });
    connect(timeline_, &TimelineDock::onionChanged, this, [this] { canvas_->markDirty(); });
    connect(timeline_, &TimelineDock::keyChanged, this, [this] {
        timeline_->refresh();
        canvas_->markDirty();
        layerPanel_->refresh();
    });
    connect(timeline_, &TimelineDock::layersChanged, this, [this] {
        timeline_->refresh();
        canvas_->markDirty();
        layerPanel_->refresh();
    });
    connect(timeline_, &TimelineDock::currentLayerChanged, this, [this] { layerPanel_->refresh(); });
    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::onViewChanged);
    // A tool committed an undoable edit: the canvas already holds the new
    // pixels via its incremental path (no rebuild needed), so only the panels
    // sync here — scoped to the edited cell to avoid re-rendering the whole
    // filmstrip synchronously on release (the click-release jank).
    connect(toolCtx_, &ToolContext::strokeCommitted, this, [this] {
        QString trackId;
        int key = -1;
        uint64_t cellLayerId = 0;
        if (PaintLayer *l = doc_->currentLayer()) {
            trackId = QString::fromStdString(l->stableId());
            key = l->exposureAt(doc_->currentFrame());
            if (CellLayer *a = doc_->currentCellLayer())
                cellLayerId = a->id();
        }
        timeline_->refreshPixels(trackId, key);
        layerPanel_->refresh();
        if (cellLayerId)
            layerPanel_->invalidateThumb(cellLayerId);
        else
            layerPanel_->invalidateThumbs();
    });
    // A tool recorded keyframes instead of pixels: the canvas has already
    // rebuilt, but the timeline has to show the new keys (and, when it is open,
    // the easing graph has to redraw them).
    connect(toolCtx_, &ToolContext::animationRecorded, this, [this] {
        timeline_->refreshContent();
        timeline_->grid()->noteAnimationChanged();
    });
    // A camera drag writes keys mid-gesture: repaint the lane's diamonds now
    // (cheap update, no thumbnail invalidation) instead of on next hover.
    connect(toolCtx_, &ToolContext::cameraPreviewChanged, this, [this] {
        timeline_->grid()->refresh();
    });
    // The panel changed the cell's layer stack: repaint the frame and the
    // timeline (its thumbnails show the composited cell).
    connect(layerPanel_, &LayerPanel::cellLayersChanged, this, [this] {
        timeline_->refreshContent();
        canvas_->markDirty();
        layerPanel_->invalidateThumbs();
    });

    tools_->setActiveTool(QStringLiteral("brush"));
    resize(1500, 950);
}

MainWindow::~MainWindow() { delete doc_; }

void MainWindow::onZoomPicked(double z) { canvas_->setZoom(z); }
void MainWindow::onRotatePicked(double deg) { canvas_->setRotation(deg); }
void MainWindow::onViewChanged() {
    topBar_->setZoom(canvas_->zoom());
    topBar_->setRotation(canvas_->rotationDeg());
}

void MainWindow::onUndo() {
    doc_->undoEdit();
    doc_->frameCache().invalidateAll();
    timeline_->refreshContent();
    canvas_->markDirty();
    layerPanel_->refresh();
    layerPanel_->invalidateThumbs();
}
void MainWindow::onRedo() {
    doc_->redoEdit();
    doc_->frameCache().invalidateAll();
    timeline_->refreshContent();
    canvas_->markDirty();
    layerPanel_->refresh();
    layerPanel_->invalidateThumbs();
}

void MainWindow::onExportPngSeq() {
    QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("PNG sequence folder"));
    if (dir.isEmpty()) return;
    for (int f = 0; f < doc_->timeline().frameCount(); ++f) {
        PaintDevice proj;
        doc_->image().project(f, proj);
        Rect e = proj.extent();
        QImage img(doc_->image().width(), doc_->image().height(), QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        if (!e.isEmpty())
            for (int y = 0; y < e.h; ++y)
                for (int x = 0; x < e.w; ++x) {
                    PixelRGBA8 p = proj.pixel(e.x+x, e.y+y);
                    img.setPixel(e.x+x, e.y+y, qRgba(p.r,p.g,p.b,p.a));
                }
        img.save(dir + QStringLiteral("/frame_%1.png").arg(f, 4, 10, QChar('0')));
    }
}

void MainWindow::onExportMp4() {
    QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Export MP4"), QStringLiteral("out.mp4"));
    if (out.isEmpty()) return;
    QString tmp = QDir::tempPath() + QStringLiteral("/inkframes");
    QDir().mkpath(tmp);
    for (int f = 0; f < doc_->timeline().frameCount(); ++f) {
        PaintDevice proj;
        doc_->image().project(f, proj);
        QImage img(doc_->image().width(), doc_->image().height(), QImage::Format_RGBA8888);
        img.fill(Qt::transparent);
        Rect e = proj.extent();
        if (!e.isEmpty())
            for (int y = 0; y < e.h; ++y)
                for (int x = 0; x < e.w; ++x) {
                    PixelRGBA8 p = proj.pixel(e.x+x, e.y+y);
                    img.setPixel(e.x+x, e.y+y, qRgba(p.r,p.g,p.b,p.a));
                }
        img.save(tmp + QStringLiteral("/frame_%1.png").arg(f, 4, 10, QChar('0')));
    }
    auto args = buildFfmpegArgs(doc_->timeline().fps(),
        (tmp + "/frame_%04d.png").toStdString(), out.toStdString());
    QStringList qargs;
    for (size_t i = 1; i < args.size(); ++i) qargs << QString::fromStdString(args[i]);
    QProcess::execute(QString::fromStdString(args[0]), qargs);
}

} // namespace ink
