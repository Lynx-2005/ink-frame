#include "ui/InkMainWindow.h"
#include "io/InkProject.h"
#include "resources/InkResources.h"
#include "ui/InkCanvasWidget.h"
#include "ui/InkDocument.h"
#include "ui/InkTheme.h"
#include "ui/InkTimelineDock.h"
#include "ui/InkToolOptions.h"
#include "ui/InkToolRail.h"
#include "ui/InkTopBar.h"
#include <QDockWidget>
#include <QDir>
#include <QFileDialog>
#include <QMenuBar>
#include <QProcess>
#include <QSplitter>
#include <QVBoxLayout>

namespace ink {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setStyleSheet(InkTheme::styleSheet());
    doc_ = new Document(1920, 1080, 12); // concept art runs 12 fps
    canvas_ = new CanvasWidget(doc_, this);

    ResourceDb db = ResourceDb::defaults();
    canvas_->setBrushPreset(*db.find("ink_pen"));

    // --- top bar ---
    topBar_ = new TopBar(this);
    connect(topBar_, &TopBar::undoRequested, this, &MainWindow::onUndo);
    connect(topBar_, &TopBar::redoRequested, this, &MainWindow::onRedo);
    connect(topBar_, &TopBar::zoomPicked, this, &MainWindow::onZoomPicked);
    connect(topBar_, &TopBar::menuRequested, this, [this] { menuBar()->setVisible(!menuBar()->isVisible()); });

    // --- left tool rail ---
    rail_ = new ToolRail(this);
    connect(rail_, &ToolRail::toolChanged, this, [this](ToolId t) { onToolChanged(int(t)); });
    connect(rail_, &ToolRail::colorsChanged, this, &MainWindow::onColorsChanged);

    // --- center layout: rail | canvas ---
    auto *center = new QWidget(this);
    auto *hl = new QHBoxLayout(center);
    hl->setContentsMargins(8, 8, 8, 8);
    hl->setSpacing(8);
    hl->addWidget(rail_);
    auto *canvasCard = new QWidget(center);
    canvasCard->setObjectName(QStringLiteral("Card"));
    auto *cl = new QVBoxLayout(canvasCard);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    toolOpts_ = new ToolOptions(canvasCard);
    toolOpts_->setBrushSize(canvas_->brushSize());
    cl->addWidget(toolOpts_);
    cl->addWidget(canvas_, 1);
    hl->addWidget(canvasCard, 1);

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

    auto *file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("Export PNG sequence…"), this, &MainWindow::onExportPngSeq);
    file->addAction(QStringLiteral("Export MP4 (ffmpeg)…"), this, &MainWindow::onExportMp4);
    auto *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(QStringLiteral("Undo"), this, &MainWindow::onUndo)->setShortcut(QKeySequence::Undo);
    edit->addAction(QStringLiteral("Redo"), this, &MainWindow::onRedo)->setShortcut(QKeySequence::Redo);
    menuBar()->setVisible(false); // hamburger in TopBar toggles

    connect(timeline_, &TimelineDock::frameChanged, this, [this](int) { canvas_->markDirty(); });
    connect(timeline_, &TimelineDock::onionChanged, this, [this] { canvas_->markDirty(); });
    connect(timeline_, &TimelineDock::keyChanged, this, [this] { timeline_->refresh(); canvas_->markDirty(); });
    connect(timeline_, &TimelineDock::layersChanged, this, [this] { timeline_->refresh(); canvas_->markDirty(); });
    connect(canvas_, &CanvasWidget::strokeFinished, this, [this] {
        timeline_->refreshContent(); canvas_->markDirty();
    });
    connect(canvas_, &CanvasWidget::viewChanged, this, &MainWindow::onViewChanged);
    connect(toolOpts_, &ToolOptions::brushSizeChanged, this, [this](double px) { canvas_->setBrushSize(px); });
    connect(toolOpts_, &ToolOptions::shapeModeChanged, this, [this](ShapeMode m) { canvas_->setShapeMode(m); });
    connect(canvas_, &CanvasWidget::colorPicked, this, [this](const QColor &c) {
        rail_->setPrimaryColor(c); // rail emits colorsChanged -> setBrushColor
    });
    resize(1500, 950);
}

MainWindow::~MainWindow() { delete doc_; }

void MainWindow::onToolChanged(int tool) {
    ToolId t = ToolId(tool);
    canvas_->setToolMode(t);
    toolOpts_->setTool(t);
    static ResourceDb s_db = ResourceDb::defaults();
    if (t == ToolId::Eraser) {
        canvas_->setEraser(true);
        if (auto *p = s_db.find("ink_eraser")) {
            BrushPreset pr = *p;
            pr.tip.diameter = float(canvas_->brushSize());
            canvas_->setBrushPreset(pr);
        }
    } else {
        canvas_->setEraser(false);
        if (t == ToolId::Brush || t == ToolId::Shape || t == ToolId::Picker) {
            if (auto *p = s_db.find("ink_pen")) {
                // keep current rail color + strip size
                BrushPreset pr = *p;
                pr.color = {uint8_t(rail_->primaryColor().red()),
                            uint8_t(rail_->primaryColor().green()),
                            uint8_t(rail_->primaryColor().blue()), 255};
                pr.tip.diameter = float(canvas_->brushSize());
                canvas_->setBrushPreset(pr);
            }
        }
    }
}

void MainWindow::onColorsChanged(QColor a, QColor) { canvas_->setBrushColor(a); }

void MainWindow::onZoomPicked(double z) { canvas_->setZoom(z); }
void MainWindow::onViewChanged() { topBar_->setZoom(canvas_->zoom()); }

void MainWindow::onUndo() { doc_->undo().undo(); doc_->frameCache().invalidateAll(); timeline_->refreshContent(); canvas_->markDirty(); }
void MainWindow::onRedo() { doc_->undo().redo(); doc_->frameCache().invalidateAll(); timeline_->refreshContent(); canvas_->markDirty(); }

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
