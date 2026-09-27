#include "ui/InkTimelineDock.h"
#include "ui/InkDocument.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include "ui/InkCurveEditor.h"
#include "ui/InkTimelineWidget.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QScrollBar>
#include <QSplitter>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>

namespace ink {

namespace {
// Hard ceiling on project length, matching TimelineModel/Image (100000).
constexpr int kMaxFrames = 100000;

QFrame *makeSep(QWidget *parent) {
    auto *f = new QFrame(parent);
    f->setObjectName(QStringLiteral("Sep"));
    f->setFrameShape(QFrame::NoFrame);
    f->setFixedWidth(1);
    f->setFixedHeight(18);
    return f;
}
QString fmtTC(int frame, int fps) {
    int s = frame / fps, f = frame % fps, m = s / 60;
    s %= 60;
    return QStringLiteral("%1:%2:%3")
        .arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(f, 2, 10, QChar('0'));
}
} // namespace

int TimelineDock::pxToSlider(double px) {
    double t = (std::log(px) - std::log(3.0)) / (std::log(64.0) - std::log(3.0));
    return int(std::clamp(t, 0.0, 1.0) * 100.0 + 0.5);
}
double TimelineDock::sliderToPx(int v) {
    double t = std::clamp(v, 0, 100) / 100.0;
    return 3.0 * std::pow(64.0 / 3.0, t);
}

TimelineDock::TimelineDock(Document *doc, QWidget *parent)
    : QDockWidget(QStringLiteral("Timeline"), parent), doc_(doc) {
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("Card"));
    auto *lay = new QVBoxLayout(root);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(4);

    // ---------------------------------------------------------------- toolbar
    auto *bar = new QWidget(root);
    bar->setObjectName(QStringLiteral("TimelineToolbar"));
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(6, 2, 6, 2);
    bl->setSpacing(3);

    auto mkBtn = [&](IconId id, const QString &tip, const QString &key) {
        auto *b = new QToolButton(bar);
        b->setIcon(InkIcons::get(id, InkTheme::dim(), 16));
        b->setToolTip(key.isEmpty() ? tip : tip + QStringLiteral("  (%1)").arg(key));
        if (!key.isEmpty()) b->setShortcut(QKeySequence(key));
        b->setAutoRaise(true);
        return b;
    };

    // transport
    auto *skipA = mkBtn(IconId::SkipStart, QStringLiteral("Go to range start"), {});
    auto *prevC = mkBtn(IconId::PrevKey, QStringLiteral("Previous cell"), QStringLiteral(","));
    playBtn_ = mkBtn(IconId::Play, QStringLiteral("Play / pause"), QStringLiteral("Space"));
    playBtn_->setObjectName(QStringLiteral("TransportPlay"));
    playBtn_->setIcon(InkIcons::get(IconId::Play, InkTheme::text(), 18));
    playBtn_->setCheckable(true);
    auto *nextC = mkBtn(IconId::NextKey, QStringLiteral("Next cell"), QStringLiteral("."));
    auto *skipB = mkBtn(IconId::SkipEnd, QStringLiteral("Go to range end"), {});

    // cells
    auto *newCell = mkBtn(IconId::CellPlus,
                          QStringLiteral("New cell at the playhead "
                                         "(inserts after the cell under it)"),
                          QStringLiteral("N"));
    auto *dupCell = mkBtn(IconId::CellDuplicate, QStringLiteral("Duplicate the active cell"), QStringLiteral("D"));
    auto *delCell = mkBtn(IconId::CellRemove, QStringLiteral("Delete the cell at the playhead"), QStringLiteral("Del"));

    // timecode + frame register
    timeLabel_ = new QLabel(bar);
    timeLabel_->setObjectName(QStringLiteral("Timecode"));
    totalLabel_ = new QLabel(bar);
    totalLabel_->setObjectName(QStringLiteral("Dim"));
    frameSpin_ = new QSpinBox(bar);
    frameSpin_->setToolTip(QStringLiteral("Current frame"));
    frameSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    frameSpin_->setFixedWidth(64);

    // playback range
    auto *inLab = new QLabel(QStringLiteral("In"), bar);
    inLab->setObjectName(QStringLiteral("Capt"));
    startSpin_ = new QSpinBox(bar);
    startSpin_->setToolTip(QStringLiteral("Playback range start (typing past the "
                                          "end extends the project)"));
    startSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    startSpin_->setRange(0, kMaxFrames - 1);
    startSpin_->setFixedWidth(64);
    auto *outLab = new QLabel(QStringLiteral("Out"), bar);
    outLab->setObjectName(QStringLiteral("Capt"));
    endSpin_ = new QSpinBox(bar);
    endSpin_->setToolTip(QStringLiteral("Playback range end (typing past the "
                                        "end extends the project)"));
    endSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    endSpin_->setRange(0, kMaxFrames - 1);
    endSpin_->setFixedWidth(64);
    auto *lenLab = new QLabel(QStringLiteral("Length"), bar);
    lenLab->setObjectName(QStringLiteral("Capt"));
    lengthSpin_ = new QSpinBox(bar);
    lengthSpin_->setToolTip(QStringLiteral("Project length in frames"));
    lengthSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    lengthSpin_->setRange(1, kMaxFrames);
    lengthSpin_->setFixedWidth(74);

    // playback config
    fpsBox_ = new QComboBox(bar);
    for (int f : {12, 24, 30, 60}) fpsBox_->addItem(QString::number(f) + QStringLiteral(" fps"), f);
    fpsBox_->setToolTip(QStringLiteral("Frame rate"));
    fpsBox_->setFixedWidth(76);
    loopBtn_ = mkBtn(IconId::Loop, QStringLiteral("Loop mode: Loop"), {});
    autoKeyBtn_ = mkBtn(IconId::Key, QStringLiteral("Auto-key: painting creates a cell"), {});
    autoKeyBtn_->setCheckable(true);
    autoKeyBtn_->setChecked(true);
    onionBtn_ = mkBtn(IconId::Onion, QStringLiteral("Onion skin — right-click for settings"), {});
    onionBtn_->setCheckable(true);
    onionBtn_->setChecked(doc->onion().enabled);
    onionBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
    followBtn_ = mkBtn(IconId::FollowHead, QStringLiteral("Follow the playhead during playback"), {});
    followBtn_->setCheckable(true);
    followBtn_->setChecked(true);

    // keyframes, in the order they are used: previous, add, next, preset, graph
    prevKeyBtn_ = mkBtn(IconId::KeyPrev, QStringLiteral("Previous keyframe on the working layer"),
                        QStringLiteral("Alt+Left"));
    addKeyBtn_ = mkBtn(IconId::KeyPlus,
                       QStringLiteral("Add a keyframe for the working layer at the playhead"),
                       QStringLiteral("K"));
    nextKeyBtn_ = mkBtn(IconId::KeyNext, QStringLiteral("Next keyframe on the working layer"),
                        QStringLiteral("Alt+Right"));
    delKeyBtn_ = mkBtn(IconId::KeyRemove,
                       QStringLiteral("Remove the keyframes at the playhead"),
                       QStringLiteral("Alt+K"));
    // (The keyframe interpolation PRESET button was removed: the same presets
    // live in the keyframe right-click menu and as thumbnails in the easing
    // graph — one home per concept instead of three.)
    graphBtn_ = mkBtn(IconId::Graph, QStringLiteral("Easing graph (shape the ease between two keyframes)"),
                      QStringLiteral("G"));
    graphBtn_->setCheckable(true);

    // view
    zoomSlider_ = new QSlider(Qt::Horizontal, bar);
    zoomSlider_->setRange(0, 100);
    zoomSlider_->setFixedWidth(90);
    zoomSlider_->setToolTip(QStringLiteral("Timeline zoom (Ctrl+wheel also works)"));
    zoomLabel_ = new QLabel(bar);
    zoomLabel_->setObjectName(QStringLiteral("Dim"));
    zoomLabel_->setMinimumWidth(52);
    auto *fitB = mkBtn(IconId::Fit, QStringLiteral("Fit the frame range"), QStringLiteral("Shift+F"));

    bl->addWidget(skipA);
    bl->addWidget(prevC);
    bl->addWidget(playBtn_);
    bl->addWidget(nextC);
    bl->addWidget(skipB);
    bl->addWidget(makeSep(bar));
    bl->addWidget(newCell);
    bl->addWidget(dupCell);
    bl->addWidget(delCell);
    bl->addWidget(makeSep(bar));
    bl->addWidget(timeLabel_);
    bl->addWidget(totalLabel_);
    bl->addWidget(frameSpin_);
    bl->addWidget(makeSep(bar));
    bl->addWidget(inLab);
    bl->addWidget(startSpin_);
    bl->addWidget(outLab);
    bl->addWidget(endSpin_);
    bl->addWidget(lenLab);
    bl->addWidget(lengthSpin_);
    bl->addWidget(makeSep(bar));
    bl->addWidget(fpsBox_);
    bl->addWidget(makeSep(bar));
    bl->addWidget(autoKeyBtn_);
    bl->addWidget(onionBtn_);
    bl->addWidget(loopBtn_);
    bl->addWidget(followBtn_);
    bl->addWidget(makeSep(bar));
    bl->addWidget(prevKeyBtn_);
    bl->addWidget(addKeyBtn_);
    bl->addWidget(nextKeyBtn_);
    bl->addWidget(delKeyBtn_);
    bl->addWidget(graphBtn_);
    bl->addStretch(1);
    bl->addWidget(zoomSlider_);
    bl->addWidget(zoomLabel_);
    bl->addWidget(fitB);
    lay->addWidget(bar);

    // --- grid + scrollbars (Krita slotRealignScrollBars pattern) ---
    auto *mid = new QWidget(root);
    auto *ml = new QHBoxLayout(mid);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(4);
    grid_ = new TimelineWidget(doc, mid);
    vBar_ = new QScrollBar(Qt::Vertical, mid);
    ml->addWidget(grid_, 1);
    ml->addWidget(vBar_);

    curve_ = new CurveEditor(doc, root);
    curve_->hide();

    // The graph docks to the RIGHT of the grid, not under it: the timeline
    // keeps its full height for rows, and the graph reads as a compact
    // inspector beside them (Alight Motion's layout, in InkFrame's chrome).
    split_ = new QSplitter(Qt::Horizontal, root);
    split_->setObjectName(QStringLiteral("TimelineSplit"));
    split_->setChildrenCollapsible(false);
    split_->setHandleWidth(4);
    split_->addWidget(mid);
    split_->addWidget(curve_);
    split_->setStretchFactor(0, 1);
    split_->setStretchFactor(1, 0);
    split_->setSizes({900, 340}); // graph starts ~340px wide, user-resizable
    lay->addWidget(split_, 1);

    // --- footer: horizontal scroll + status strip ---
    auto *bottom = new QWidget(root);
    auto *bbl = new QHBoxLayout(bottom);
    bbl->setContentsMargins(0, 0, 0, 0);
    bbl->setSpacing(8);
    hBar_ = new QScrollBar(Qt::Horizontal, bottom);
    statusLabel_ = new QLabel(bottom);
    statusLabel_->setObjectName(QStringLiteral("TimelineStatus"));
    bbl->addWidget(hBar_, 1);
    bbl->addWidget(statusLabel_);
    lay->addWidget(bottom);

    // Onion settings live on the button's right-click (no dropdown arrow).
    connect(onionBtn_, &QToolButton::customContextMenuRequested, this, &TimelineDock::showOnionMenu);

    setWidget(root);
    setMinimumHeight(260);

    const int fc = doc->timeline().frameCount();
    frameSpin_->setRange(0, fc - 1);
    startSpin_->setRange(0, kMaxFrames - 1);
    endSpin_->setRange(0, kMaxFrames - 1);
    startSpin_->setValue(0);
    endSpin_->setValue(fc - 1);
    lengthSpin_->setValue(fc);
    grid_->setRange(0, fc - 1);
    for (int i = 0; i < fpsBox_->count(); ++i)
        if (fpsBox_->itemData(i).toInt() == doc->timeline().fps()) { fpsBox_->setCurrentIndex(i); break; }

    connect(skipA, &QToolButton::clicked, this, &TimelineDock::onSkipStart);
    connect(skipB, &QToolButton::clicked, this, &TimelineDock::onSkipEnd);
    connect(prevC, &QToolButton::clicked, this, &TimelineDock::onPrevCell);
    connect(nextC, &QToolButton::clicked, this, &TimelineDock::onNextCell);
    connect(playBtn_, &QToolButton::toggled, this, &TimelineDock::onPlayToggle);
    connect(newCell, &QToolButton::clicked, this, &TimelineDock::onNewCell);
    connect(dupCell, &QToolButton::clicked, this, &TimelineDock::onDuplicateCell);
    connect(delCell, &QToolButton::clicked, this, &TimelineDock::onDeleteCell);
    connect(fpsBox_, &QComboBox::currentIndexChanged, this, &TimelineDock::onFpsChanged);
    connect(loopBtn_, &QToolButton::clicked, this, &TimelineDock::onLoopCycle);
    connect(onionBtn_, &QToolButton::toggled, this, &TimelineDock::onOnionToggled);
    connect(followBtn_, &QToolButton::toggled, this, &TimelineDock::onFollowToggled);
    connect(autoKeyBtn_, &QToolButton::toggled, this, &TimelineDock::onAutoKeyToggled);
    doc->setAutoKeyEnabled(autoKeyBtn_->isChecked());
    connect(frameSpin_, &QSpinBox::valueChanged, this, &TimelineDock::onFrameRegisterChanged);
    connect(startSpin_, &QSpinBox::valueChanged, this, &TimelineDock::onRangeChanged);
    connect(endSpin_, &QSpinBox::valueChanged, this, &TimelineDock::onRangeChanged);
    connect(lengthSpin_, &QSpinBox::valueChanged, this, &TimelineDock::onLengthChanged);
    connect(zoomSlider_, &QSlider::valueChanged, this, &TimelineDock::onZoomSlider);
    connect(fitB, &QToolButton::clicked, this, &TimelineDock::onFitView);
    connect(hBar_, &QScrollBar::valueChanged, this, [this](int v) { grid_->setScroll(v, vBar_->value()); });
    connect(vBar_, &QScrollBar::valueChanged, this, [this](int v) { grid_->setScroll(hBar_->value(), v); });
    connect(grid_, &TimelineWidget::frameChanged, this, [this](int f) {
        updateTimecode();
        updateStatus();
        emit frameChanged(f);
    });
    connect(grid_, &TimelineWidget::keyChanged, this, [this] {
        grid_->refresh();
        syncBars();
        updateStatus();
        emit keyChanged();
    });
    connect(grid_, &TimelineWidget::selectionChanged, this, [this] { updateStatus(); emit keyChanged(); });
    connect(grid_, &TimelineWidget::layerSelectionChanged, this, [this] {
        updateStatus();
        syncKeyButtons();
        syncGraphTarget();
        emit currentLayerChanged();
    });
    connect(grid_, &TimelineWidget::scrollRequest, this, &TimelineDock::onGridScroll);
    connect(grid_, &TimelineWidget::zoomChanged, this, &TimelineDock::onGridZoom);
    connect(grid_, &TimelineWidget::rangeChangeRequested, this, &TimelineDock::onGridRange);
    connect(grid_, &TimelineWidget::trackMenuRequested, this, &TimelineDock::showTrackMenu);
    connect(grid_, &TimelineWidget::laneMenuRequested, this, &TimelineDock::showLaneMenu);
    connect(grid_, &TimelineWidget::rulerMenuRequested, this, &TimelineDock::showRulerMenu);
    // --- keyframes ---
    connect(addKeyBtn_, &QToolButton::clicked, this, &TimelineDock::onAddKey);
    connect(delKeyBtn_, &QToolButton::clicked, this, &TimelineDock::onRemoveKey);
    connect(prevKeyBtn_, &QToolButton::clicked, this, &TimelineDock::onPrevKey);
    connect(nextKeyBtn_, &QToolButton::clicked, this, &TimelineDock::onNextKey);
    connect(graphBtn_, &QToolButton::toggled, this, &TimelineDock::onGraphToggled);
    connect(grid_, &TimelineWidget::keyFrameMenuRequested, this, &TimelineDock::onKeyMenu);
    // --- the easing graph -------------------------------------------------
    // It edits ONE segment of ONE parameter in a normalised box, so it shares no
    // time axis with the grid: the only coupling is that paging a segment moves
    // the playhead, and the playhead picks which segment is shown.
    connect(curve_, &CurveEditor::graphChanged, this, [this] {
        grid_->refresh();
        updateTimecode();
        updateStatus();
        emit keyChanged();
    });
    connect(curve_, &CurveEditor::statusMessage, this, [this](const QString &t) {
        if (statusLabel_) statusLabel_->setText(t);
    });
    syncKeyButtons();
    connect(grid_, &TimelineWidget::addLayerRequested, this, &TimelineDock::onAddLayer);
    connect(grid_, &TimelineWidget::addGroupRequested, this, &TimelineDock::onAddGroup);
    connect(grid_, &TimelineWidget::groupSelectionRequested, this, &TimelineDock::onGroupSelected);
    connect(grid_, &TimelineWidget::ungroupRequested, this, &TimelineDock::onUngroup);
    connect(grid_, &TimelineWidget::renameRequested, this, &TimelineDock::onRename);
    connect(grid_, &TimelineWidget::addCellRequested, this, [this](const QString &trackId, int frame) {
        if (PaintLayer *l = doc_->findTrack(trackId.toStdString())) {
            if (doc_->addBlankKey(l, frame)) {
                doc_->setCurrentLayer(l);
                refreshContent();
                emit keyChanged();
            }
        }
    });
    connect(grid_, &TimelineWidget::insertCellRequested, this,
            [this](const QString &trackId, int atFrame) {
                PaintLayer *l = doc_->findTrack(trackId.toStdString());
                if (!l || l->locked()) return;
                if (doc_->insertCell(l, atFrame)) {
                    doc_->setCurrentLayer(l);
                    doc_->setCurrentFrame(atFrame);
                    grid_->setSelection({{trackId, atFrame}});
                    refreshContent();
                    emit keyChanged();
                }
            });

    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &TimelineDock::tick);
    timer_->start();
    syncZoom();
    syncBars();
    syncTransport();
    updateTimecode();
    updateStatus();
}

void TimelineDock::refresh() {
    grid_->refresh();
    if (curve_ && !curve_->isHidden()) curve_->refresh();
    syncBars();
    syncZoom();
    updateTimecode();
    syncKeyButtons();
    updateStatus();
}

void TimelineDock::refreshContent() {
    grid_->invalidateCells();
    refresh();
}

void TimelineDock::refreshPixels(const QString &trackId, int key) {
    grid_->invalidateCell(trackId, key);
    refresh();
}

void TimelineDock::updateTimecode() {
    const int fc = doc_->timeline().frameCount(), cur = doc_->timeline().currentFrame();
    const int fps = std::max(1, doc_->timeline().fps());
    frameSpin_->blockSignals(true);
    frameSpin_->setValue(cur);
    frameSpin_->blockSignals(false);
    timeLabel_->setText(fmtTC(cur, fps));
    totalLabel_->setText(QStringLiteral("/ ") + fmtTC(fc - 1, fps));
}

void TimelineDock::updateStatus() {
    int layers = 0, cameras = 0, folders = 0;
    for (const auto &f : doc_->timeline().folders()) {
        ++folders;
        for (const auto &r : f.tracks)
            if (r.isCamera) ++cameras;
            else ++layers;
    }
    const int selCells = grid_->selection().size();
    const int selLayers = grid_->selectedLayers().size();
    QString cellInfo = QStringLiteral("no cell");
    if (PaintLayer *l = doc_->currentLayer()) {
        const std::string id = l->stableId();
        const int idx = doc_->timeline().cellIndexAt(id, doc_->currentFrame());
        const int total = doc_->timeline().cellCount(id);
        if (total > 0 && idx >= 0)
            cellInfo = QStringLiteral("cell %1/%2").arg(idx + 1).arg(total);
        else if (total > 0)
            cellInfo = QStringLiteral("%1 cells").arg(total);
    }
    QStringList parts;
    parts << (layers == 1 ? QStringLiteral("1 layer") : QStringLiteral("%1 layers").arg(layers));
    if (cameras > 0)
        parts << (cameras == 1 ? QStringLiteral("1 camera")
                               : QStringLiteral("%1 cameras").arg(cameras));
    if (folders > 1) parts << QStringLiteral("%1 folders").arg(folders);
    if (selCells > 0) parts << QStringLiteral("%1 cells selected").arg(selCells);
    if (selLayers > 0)
        parts << (selLayers == 1 ? QStringLiteral("1 layer selected")
                                 : QStringLiteral("%1 layers selected").arg(selLayers));
    parts << cellInfo;
    if (CameraLayer *camSt = keyCamera()) {
        const int keys = camSt->params().keyCount();
        parts << (keys == 1 ? QStringLiteral("1 camera key")
                            : QStringLiteral("%1 camera keys").arg(keys));
    } else if (AdjustmentLayer *a = keyLayer()) {
        // Short on purpose: the strip is a glance, and the layer's full name is
        // already in the gutter and in the button tooltips.
        const int keys = a->params().keyCount();
        parts << (keys == 1 ? QStringLiteral("1 key") : QStringLiteral("%1 keys").arg(keys));
    }
    parts << QStringLiteral("%1 frames").arg(doc_->timeline().frameCount());
    parts << QStringLiteral("%1 fps").arg(doc_->timeline().fps());
    const QString line = parts.join(QStringLiteral("  ·  "));
    statusLabel_->setText(line);
    // The strip is the first thing a narrow window clips, and a clipped status
    // is worse than no status: keep the whole line reachable.
    statusLabel_->setToolTip(line);
}

void TimelineDock::syncBars() {
    int cw = grid_->contentWidth(), ch = grid_->contentHeight();
    int vw = grid_->width(), vh = grid_->height();
    hBar_->blockSignals(true);
    hBar_->setRange(0, std::max(0, cw - vw));
    hBar_->setPageStep(std::max(10, vw - InkTheme::gutterW()));
    hBar_->setSingleStep(20);
    hBar_->setValue(std::clamp(grid_->hScroll(), 0, hBar_->maximum()));
    hBar_->blockSignals(false);
    vBar_->blockSignals(true);
    vBar_->setRange(0, std::max(0, ch - vh));
    vBar_->setPageStep(std::max(10, vh / 2));
    vBar_->setSingleStep(InkTheme::trackH());
    vBar_->blockSignals(false);
    grid_->setScroll(hBar_->value(), vBar_->value());
}

void TimelineDock::syncZoom() {
    zoomSlider_->blockSignals(true);
    zoomSlider_->setValue(pxToSlider(grid_->pxPerFrame()));
    zoomSlider_->blockSignals(false);
    zoomLabel_->setText(QString::number(int(grid_->pxPerFrame() * 10) / 10.0, 'f', 1) + QStringLiteral(" px/f"));
}

void TimelineDock::syncTransport() {
    const bool playing = doc_->playback().isPlaying();
    playBtn_->blockSignals(true);
    playBtn_->setChecked(playing);
    playBtn_->blockSignals(false);
    playBtn_->setIcon(InkIcons::get(playing ? IconId::Pause : IconId::Play,
                                   playing ? InkTheme::accent() : InkTheme::text(), 18));
}

void TimelineDock::onGridScroll(int hx, int vy) {
    hBar_->blockSignals(true);
    vBar_->blockSignals(true);
    hBar_->setValue(std::clamp(hx, 0, hBar_->maximum()));
    vBar_->setValue(std::clamp(vy, 0, vBar_->maximum()));
    hBar_->blockSignals(false);
    vBar_->blockSignals(false);
    grid_->setScroll(hBar_->value(), vBar_->value());
}

void TimelineDock::onGridZoom(double) { syncZoom(); syncBars(); }
void TimelineDock::onZoomSlider(int v) { grid_->setPxPerFrame(sliderToPx(v), grid_->width() / 2); syncBars(); }
void TimelineDock::onFitView() { grid_->fitToRange(); syncBars(); syncZoom(); }

void TimelineDock::onGridRange(int in, int out) {
    startSpin_->blockSignals(true);
    endSpin_->blockSignals(true);
    startSpin_->setValue(in);
    endSpin_->setValue(out);
    startSpin_->blockSignals(false);
    endSpin_->blockSignals(false);
    doc_->playback().setRange(in, out);
}

void TimelineDock::onPlayToggle() {
    if (playBtn_->isChecked()) {
        doc_->playback().setRange(startSpin_->value(), endSpin_->value());
        doc_->playback().play();
    } else {
        doc_->playback().pause();
    }
    syncTransport();
}

void TimelineDock::onSkipStart() {
    doc_->playback().pause();
    const int f = startSpin_->value();
    doc_->setCurrentFrame(f);
    grid_->ensureFrameVisible(f);
    refresh();
    emit frameChanged(f);
}

void TimelineDock::onSkipEnd() {
    doc_->playback().pause();
    const int f = endSpin_->value();
    doc_->setCurrentFrame(f);
    grid_->ensureFrameVisible(f);
    refresh();
    emit frameChanged(f);
}

// Cells: jump to the nearest cell boundary across every track.
void TimelineDock::onPrevCell() {
    const int cur = doc_->currentFrame();
    int best = -1;
    for (const auto &r : doc_->timeline().rows()) {
        int p = r.frames.prevKey(cur);
        if (p > best) best = p;
    }
    if (best >= 0) {
        doc_->setCurrentFrame(best);
        if (follow_) grid_->ensureFrameVisible(best);
        refresh();
        emit frameChanged(best);
    }
}

void TimelineDock::onNextCell() {
    const int cur = doc_->currentFrame();
    int best = INT_MAX;
    for (const auto &r : doc_->timeline().rows()) {
        int n = r.frames.nextKey(cur);
        if (n >= 0 && n < best) best = n;
    }
    if (best != INT_MAX) {
        doc_->setCurrentFrame(best);
        if (follow_) grid_->ensureFrameVisible(best);
        refresh();
        emit frameChanged(best);
    }
}

void TimelineDock::onNewCell() {
    PaintLayer *l = doc_->currentLayer();
    if (!l) return;
    if (l->locked()) return;
    const int cur = doc_->currentFrame();
    const int cover = doc_->cellStartAt(l, cur);
    if (cover < 0) {
        if (doc_->addBlankKey(l, cur)) {
            refreshContent();
            emit keyChanged();
        }
        return;
    }
    // The playhead sits on a cell: "new cell" becomes an insert right after it,
    // so the button always lands a cell instead of refusing silently.
    const int at = doc_->cellEnd(l, cover) + 1;
    if (doc_->insertCell(l, at)) {
        doc_->setCurrentFrame(at);
        refreshContent();
        emit keyChanged();
    }
}

void TimelineDock::onDuplicateCell() {
    PaintLayer *l = doc_->currentLayer();
    if (!l || l->locked()) return;
    const int cur = doc_->currentFrame();
    const int src = doc_->cellStartAt(l, cur);
    if (src < 0) {
        onNewCell();
        return;
    }
    const int dst = (src == cur) ? std::min(cur + 1, doc_->timeline().frameCount() - 1) : cur;
    if (dst == cur && src == cur) return;
    if (doc_->duplicateKey(l, src, dst, false)) {
        doc_->setCurrentFrame(dst);
        refreshContent();
        emit keyChanged();
    }
}

void TimelineDock::onDeleteCell() {
    PaintLayer *l = doc_->currentLayer();
    if (!l || l->locked()) return;
    const int cur = doc_->currentFrame();
    if (!l->hasKeyAt(cur)) return;
    doc_->removeKeys(l, {cur}, false);
    refreshContent();
    emit keyChanged();
}

void TimelineDock::onFpsChanged(int idx) {
    const int fps = fpsBox_->itemData(idx).toInt();
    doc_->timeline().setFps(fps);
    doc_->playback().setFps(fps);
    doc_->image().setFps(fps);
    updateTimecode();
    grid_->refresh();
}

void TimelineDock::onLoopCycle() {
    LoopMode m = LoopMode::Loop;
    switch (doc_->playback().loop()) {
    case LoopMode::Loop: m = LoopMode::Once; break;
    case LoopMode::Once: m = LoopMode::PingPong; break;
    case LoopMode::PingPong: m = LoopMode::Loop; break;
    }
    doc_->playback().setLoop(m);
    IconId icon = IconId::Loop;
    QString name = QStringLiteral("Loop");
    if (m == LoopMode::Once) { icon = IconId::Once; name = QStringLiteral("Once"); }
    else if (m == LoopMode::PingPong) { icon = IconId::PingPong; name = QStringLiteral("Ping-pong"); }
    loopBtn_->setIcon(InkIcons::get(icon, InkTheme::dim(), 16));
    loopBtn_->setToolTip(QStringLiteral("Loop mode: %1 (click to cycle)").arg(name));
}

void TimelineDock::onOnionToggled(bool on) {
    doc_->onion().enabled = on;
    onionBtn_->setIcon(InkIcons::get(IconId::Onion, on ? InkTheme::accent() : InkTheme::dim(), 16));
    grid_->refresh();
    emit onionChanged();
    emit frameChanged(doc_->currentFrame());
}

void TimelineDock::showOnionMenu(const QPoint &pos) {
    QMenu m(this);
    QAction *on = m.addAction(QStringLiteral("Onion skin enabled"));
    on->setCheckable(true);
    on->setChecked(doc_->onion().enabled);
    connect(on, &QAction::triggered, this, [this](bool v) { onionBtn_->setChecked(v); });
    m.addSeparator();
    auto *prevRow = new QWidget(&m);
    auto *pl = new QHBoxLayout(prevRow);
    pl->setContentsMargins(8, 2, 8, 2);
    auto *prevSpin = new QSpinBox(prevRow);
    prevSpin->setRange(0, 8);
    prevSpin->setValue(doc_->onion().prevCount);
    prevSpin->setPrefix(QStringLiteral("◀ "));
    auto *nextSpin = new QSpinBox(prevRow);
    nextSpin->setRange(0, 8);
    nextSpin->setValue(doc_->onion().nextCount);
    nextSpin->setPrefix(QStringLiteral("▶ "));
    pl->addWidget(new QLabel(QStringLiteral("Frames"), prevRow));
    pl->addWidget(prevSpin);
    pl->addWidget(nextSpin);
    auto *pa = new QWidgetAction(&m);
    pa->setDefaultWidget(prevRow);
    m.addAction(pa);
    auto *opRow = new QWidget(&m);
    auto *ol = new QHBoxLayout(opRow);
    ol->setContentsMargins(8, 2, 8, 2);
    auto *opSpin = new QDoubleSpinBox(opRow);
    opSpin->setRange(0.05, 1.0);
    opSpin->setSingleStep(0.05);
    opSpin->setValue(doc_->onion().maxOpacity);
    ol->addWidget(new QLabel(QStringLiteral("Opacity"), opRow));
    ol->addWidget(opSpin);
    auto *oa = new QWidgetAction(&m);
    oa->setDefaultWidget(opRow);
    m.addAction(oa);
    QAction *tint = m.addAction(QStringLiteral("Tint previous/next frames (red / blue)"));
    tint->setCheckable(true);
    tint->setChecked(doc_->onion().tintEnabled);
    tint->setToolTip(QStringLiteral("Off: ghosts keep their own colours at reduced opacity"));
    connect(prevSpin, &QSpinBox::valueChanged, this, [this](int v) {
        doc_->onion().prevCount = v;
        grid_->refresh();
        emit onionChanged();
    });
    connect(nextSpin, &QSpinBox::valueChanged, this, [this](int v) {
        doc_->onion().nextCount = v;
        grid_->refresh();
        emit onionChanged();
    });
    connect(opSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        doc_->onion().maxOpacity = float(v);
        grid_->refresh();
        emit onionChanged();
    });
    connect(tint, &QAction::triggered, this, [this](bool v) {
        doc_->onion().tintEnabled = v;
        grid_->refresh();
        emit onionChanged();
    });
    m.exec(onionBtn_->mapToGlobal(pos));
}

void TimelineDock::onFollowToggled(bool on) {
    follow_ = on;
    followBtn_->setIcon(InkIcons::get(IconId::FollowHead, on ? InkTheme::accent() : InkTheme::dim(), 16));
}

void TimelineDock::onAutoKeyToggled(bool on) {
    autoKey_ = on;
    if (doc_) doc_->setAutoKeyEnabled(on);
    autoKeyBtn_->setIcon(InkIcons::get(IconId::Key, on ? InkTheme::accent() : InkTheme::dim(), 16));
    autoKeyBtn_->setToolTip(on ? QStringLiteral("Auto-key: painting creates a cell")
                               : QStringLiteral("Auto-key off: painting needs a key cell"));
    syncKeyButtons();
}

void TimelineDock::onFrameRegisterChanged(int f) {
    ensureFrameCount(f); // typing past the end extends the project
    doc_->setCurrentFrame(f);
    if (follow_) grid_->ensureFrameVisible(f);
    grid_->refresh();
    updateTimecode();
    updateStatus();
    emit frameChanged(f);
}

void TimelineDock::onRangeChanged() {
    // In/Out are also the way to grow a project: whatever the user types is
    // honoured instead of being clamped to the current length.
    ensureFrameCount(std::max(startSpin_->value(), endSpin_->value()));
    const int s = std::min(startSpin_->value(), endSpin_->value());
    const int e = std::max(startSpin_->value(), endSpin_->value());
    doc_->playback().setRange(s, e);
    grid_->setRange(s, e);
    syncFrameCount();
    grid_->refresh();
}

void TimelineDock::onLengthChanged(int frames) {
    const int oldLast = doc_->timeline().frameCount() - 1;
    const bool outWasPinned = (endSpin_->value() >= oldLast);
    if (!doc_->setFrameCount(frames)) return;
    // A range that ran to the old end follows the new one; anything else stays.
    endSpin_->blockSignals(true);
    if (outWasPinned) endSpin_->setValue(doc_->timeline().frameCount() - 1);
    endSpin_->blockSignals(false);
    syncFrameCount();
    grid_->refresh();
    emit keyChanged();
}

void TimelineDock::syncFrameCount() {
    const int fc = doc_->timeline().frameCount();
    frameSpin_->blockSignals(true);
    frameSpin_->setRange(0, fc - 1);
    frameSpin_->blockSignals(false);
    lengthSpin_->blockSignals(true);
    lengthSpin_->setValue(fc);
    lengthSpin_->blockSignals(false);
    // In/Out may have been clamped by a shorter project.
    const int s = std::clamp(startSpin_->value(), 0, fc - 1);
    const int e = std::clamp(endSpin_->value(), 0, fc - 1);
    startSpin_->blockSignals(true);
    endSpin_->blockSignals(true);
    startSpin_->setValue(s);
    endSpin_->setValue(e);
    startSpin_->blockSignals(false);
    endSpin_->blockSignals(false);
    grid_->setRange(s, e);
    doc_->playback().setRange(s, e);
    updateTimecode();
    updateStatus();
    syncBars();
}

void TimelineDock::ensureFrameCount(int frame) {
    if (frame < 0 || frame >= doc_->timeline().frameCount()) {
        if (!doc_->setFrameCount(frame + 1)) return;
        syncFrameCount();
        emit keyChanged();
    }
}

// ------------------------------------------------------------ layer + group

void TimelineDock::onAddLayer() {
    doc_->createTrackAboveCurrent({});
    refreshContent();
    emit layersChanged();
    emit keyChanged();
}

void TimelineDock::onAddGroup() {
    // instant, auto-named: naming stays a double-click on the folder row
    doc_->createFolder({});
    refreshContent();
    emit layersChanged();
    emit keyChanged();
}

void TimelineDock::onGroupSelected() {
    const QSet<QString> sel = grid_->selectedLayers();
    if (sel.isEmpty()) return;
    // tree order keeps the new folder's stack order predictable. Any layer kind
    // is eligible, so an adjustment layer groups with the layer it animates.
    std::vector<Layer*> members;
    const auto &folders = doc_->timeline().folders();
    for (const auto &f : folders)
        for (const auto &t : f.tracks)
            if (sel.contains(QString::fromStdString(t.layerId)))
                if (Layer *l = doc_->findLayer(t.layerId)) members.push_back(l);
    GroupLayer *g = doc_->groupSelected(members, {});
    if (!g) return;
    grid_->clearLayerSelection();
    refreshContent();
    emit layersChanged();
    emit keyChanged();
}

// --- keyframes ---------------------------------------------------------------

// The working layer for keyframe operations: the selected adjustment layer, or
// whatever adjustment layer is linked to the selected paint layer.
AdjustmentLayer *TimelineDock::keyLayer() const {
    // The keyframes live on the working layer itself; the "key layer" is its
    // compatibility view.
    if (PaintLayer *l = doc_->currentLayer()) return doc_->adjustmentFor(l);
    return nullptr;
}

CameraLayer *TimelineDock::keyCamera() const {
    for (const QString &id : grid_->selectedLayers())
        if (CameraLayer *c = doc_->findCamera(id.toStdString())) return c;
    return nullptr;
}

void TimelineDock::onAddKey() {
    const int f = doc_->currentFrame();
    if (CameraLayer *cam = keyCamera()) {
        const int n = doc_->keyCameraAt(cam, f);
        if (n <= 0) {
            if (statusLabel_)
                statusLabel_->setText(QStringLiteral("Camera is locked"));
            return;
        }
        if (!curve_->isHidden()) {
            curve_->setCamera(cam);
            curve_->refresh();
        }
        refreshContent();
        syncKeyButtons();
        if (statusLabel_)
            statusLabel_->setText(QStringLiteral("Keyed camera at frame %1").arg(f));
        emit keyChanged();
        return;
    }
    if (!keyLayer() && !doc_->currentLayer()) {
        if (statusLabel_) statusLabel_->setText(QStringLiteral("Select a layer first"));
        return;
    }
    const int n = doc_->keyCurrentLayer(f);
    if (n <= 0) {
        if (statusLabel_)
            statusLabel_->setText(QStringLiteral("Nothing to key — select a layer"));
        return;
    }
    // The keys are now visible on the layer's BAR itself (one diamond per keyed
    // frame), so there is nothing to expand; the graph follows the same layer.
    if (AdjustmentLayer *a = keyLayer()) {
        if (!curve_->isHidden()) {
            curve_->setAdjustment(a);
            curve_->refresh();
        }
    }
    refreshContent();
    syncKeyButtons();
    if (statusLabel_)
        statusLabel_->setText(QStringLiteral("Keyed %1 parameter(s) at frame %2").arg(n).arg(f));
    emit keyChanged();
}

void TimelineDock::onRemoveKey() {
    if (CameraLayer *cam = keyCamera()) {
        const int n = doc_->removeCameraKeysAt(cam, doc_->currentFrame());
        if (n > 0) {
            grid_->noteAnimationChanged();
            refreshContent();
            syncKeyButtons();
            if (statusLabel_)
                statusLabel_->setText(QStringLiteral("Removed %1 camera keyframe(s)").arg(n));
            emit keyChanged();
        } else if (statusLabel_) {
            statusLabel_->setText(QStringLiteral("No camera keyframe at the playhead"));
        }
        return;
    }
    const int n = doc_->removeKeyCurrentLayer(doc_->currentFrame());
    if (n > 0) {
        refreshContent();
        syncKeyButtons();
        if (statusLabel_)
            statusLabel_->setText(QStringLiteral("Removed %1 keyframe(s)").arg(n));
        emit keyChanged();
    } else if (statusLabel_) {
        statusLabel_->setText(QStringLiteral("No keyframe at the playhead on this layer"));
    }
}

void TimelineDock::onPrevKey() {
    if (CameraLayer *cam = keyCamera()) {
        const int f = doc_->prevCameraKey(cam, doc_->currentFrame());
        if (f < 0) {
            if (statusLabel_) statusLabel_->setText(QStringLiteral("No camera keyframe before the playhead"));
            return;
        }
        doc_->setCurrentFrame(f);
        if (follow_) grid_->ensureFrameVisible(f);
        if (!curve_->isHidden()) curve_->refresh();
        grid_->refresh();
        syncBars();
        updateTimecode();
        updateStatus();
        emit frameChanged(f);
        return;
    }
    const int f = doc_->prevKeyCurrentLayer(doc_->currentFrame());
    if (f < 0) {
        if (statusLabel_) statusLabel_->setText(QStringLiteral("No keyframe before the playhead"));
        return;
    }
    doc_->setCurrentFrame(f);
    if (follow_) grid_->ensureFrameVisible(f);
    if (!curve_->isHidden()) curve_->refresh();
    grid_->refresh();
    syncBars();
    updateTimecode();
    updateStatus();
    emit frameChanged(f);
}

void TimelineDock::onNextKey() {
    if (CameraLayer *cam = keyCamera()) {
        const int f = doc_->nextCameraKey(cam, doc_->currentFrame());
        if (f < 0) {
            if (statusLabel_) statusLabel_->setText(QStringLiteral("No camera keyframe after the playhead"));
            return;
        }
        doc_->setCurrentFrame(f);
        if (follow_) grid_->ensureFrameVisible(f);
        if (!curve_->isHidden()) curve_->refresh();
        grid_->refresh();
        syncBars();
        updateTimecode();
        updateStatus();
        emit frameChanged(f);
        return;
    }
    const int f = doc_->nextKeyCurrentLayer(doc_->currentFrame());
    if (f < 0) {
        if (statusLabel_) statusLabel_->setText(QStringLiteral("No keyframe after the playhead"));
        return;
    }
    doc_->setCurrentFrame(f);
    if (follow_) grid_->ensureFrameVisible(f);
    if (!curve_->isHidden()) curve_->refresh();
    grid_->refresh();
    syncBars();
    updateTimecode();
    updateStatus();
    emit frameChanged(f);
}

void TimelineDock::applyPreset(int interp, bool easeIn, bool easeOut) {
    AdjustmentLayer *a = keyLayer();
    if (!a) return;
    const int f = doc_->currentFrame();
    // With keys selected, the preset applies to THOSE; otherwise to the keys
    // under the playhead, which is what the button means when nothing is picked.
    QSet<TimelineKey> sel = grid_->keySelection();
    bool any = false;
    if (!sel.isEmpty()) {
        for (const TimelineKey &k : sel) {
            if (k.adjId != QString::fromStdString(a->stableId())) continue;
            any |= doc_->applyKeyPreset(a, k.frame, (Interp)interp, easeIn, easeOut);
        }
    } else {
        any = doc_->applyKeyPreset(a, f, (Interp)interp, easeIn, easeOut);
    }
    if (any) {
        refreshContent();
        if (!curve_->isHidden()) curve_->refresh();
        emit keyChanged();
    } else if (statusLabel_) {
        statusLabel_->setText(QStringLiteral("No keyframe to apply the preset to"));
    }
}

void TimelineDock::syncGraphTarget() {
    if (!curve_ || curve_->isHidden()) return;
    if (CameraLayer *gcam = keyCamera()) {
        curve_->setCamera(gcam);
    } else {
        curve_->setAdjustment(keyLayer());
    }
    curve_->refresh();
    if (graphBtn_) {
        const bool cam = keyCamera() != nullptr;
        graphBtn_->setToolTip(cam ? QStringLiteral("Camera easing graph (shape the ease between two "
                                                   "camera keyframes)  (G)")
                                 : QStringLiteral("Easing graph (shape the ease between two keyframes)  (G)"));
    }
}

void TimelineDock::onGraphToggled(bool on) {
    curve_->setVisible(on);
    if (on) syncGraphTarget();
    if (statusLabel_)
        statusLabel_->setText(
            on ? (keyCamera()
                      ? QStringLiteral("Camera easing graph — drag a handle to shape this camera "
                                       "move, \u2190/\u2192 to page between segments")
                      : QStringLiteral("Easing graph — drag a handle to shape this segment, click a "
                                       "preset to apply it, \u2190/\u2192 to page between segments"))
               : QString());
}

// A bar diamond stands for every parameter keyed at that frame, so the menu
// acts on the frame, not on one hidden parameter row. Interpolation is applied
// to all of them at once — which is what an animator wants: one easing per beat.
// A bar diamond stands for every parameter keyed at that frame, so the menu acts
// on the frame rather than on a hidden per-parameter row. Document::applyKeyPreset
// already broadcasts over keysAt(frame), which makes each entry one undo step.
void TimelineDock::onKeyMenu(const QPoint &globalPos, const QString &adjId, int frame) {
    if (QMenu *m = buildKeyMenu(adjId, frame)) m->exec(globalPos);
}

QMenu *TimelineDock::buildKeyMenu(const QString &adjId, int frame) {
    // Cameras share the same diamond menu (keyframes live on the camera).
    if (CameraLayer *cam = doc_->findCamera(adjId.toStdString())) {
        const std::vector<ParamId> cids = cam->params().keysAt(frame);
        if (cids.empty()) return nullptr;
        auto *m = new QMenu(this);
        QAction *title = m->addAction(QStringLiteral("Camera keyframe · frame %1").arg(frame));
        title->setEnabled(false);
        m->addSeparator();
        const ScalarChannel *ch = cam->params().channel(cids.front());
        const ScalarKey *k = ch ? ch->keyAt(frame) : nullptr;
        const Interp cur = k ? k->interp : Interp::Linear;
        const bool flat = k && std::abs(k->outSpeed) < 1e-6 && std::abs(k->inSpeed) < 1e-6;
        struct EaseItem { const char *label; Interp mode; bool in; bool out; };
        static const EaseItem items[] = {
            {"Linear", Interp::Linear, false, false},
            {"Hold", Interp::Hold, false, false},
            {"Ease In", Interp::Bezier, true, false},
            {"Ease Out", Interp::Bezier, false, true},
            {"Ease In/Out", Interp::Bezier, true, true},
        };
        for (const EaseItem &e : items) {
            QAction *act = m->addAction(QString::fromUtf8(e.label));
            act->setCheckable(true);
            bool checked = (cur == e.mode);
            if (checked && e.mode == Interp::Bezier) {
                const bool zin = k && std::abs(k->inSpeed) < 1e-6;
                const bool zout = k && std::abs(k->outSpeed) < 1e-6;
                checked = (e.in && e.out) ? flat : (e.in ? (zin && !zout) : (zout && !zin));
            }
            act->setChecked(checked);
            connect(act, &QAction::triggered, this, [this, cam, frame, e] {
                doc_->applyCameraPreset(cam, frame, e.mode, e.in, e.out);
                grid_->noteAnimationChanged();
                refreshContent();
                emit keyChanged();
            });
        }
        m->addSeparator();
        QAction *del = m->addAction(QStringLiteral("Delete keyframe"));
        connect(del, &QAction::triggered, this, [this, cam, cids, frame] {
            for (ParamId id : cids) doc_->removeCameraKey(cam, id, frame);
            grid_->noteAnimationChanged();
            refreshContent();
            emit keyChanged();
        });
        m->addSeparator();
        QAction *graph = m->addAction(QStringLiteral("Open easing graph"));
        connect(graph, &QAction::triggered, this, [this, cam] {
            if (curve_->isHidden()) graphBtn_->setChecked(true);
            curve_->setCamera(cam);
            curve_->refresh();
        });
        return m;
    }
    AdjustmentLayer *a = doc_->findAdjustment(adjId.toStdString());
    if (!a) return nullptr;
    const std::vector<ParamId> ids = a->params().keysAt(frame);
    if (ids.empty()) return nullptr;

    // Heap-owned: the caller either exec()s it or reads its entries, and a
    // stack QMenu could not outlive this function.
    auto *m = new QMenu(this);
    QAction *title = m->addAction(QStringLiteral("Keyframe · frame %1").arg(frame));
    title->setEnabled(false);
    m->addSeparator();

    const ScalarChannel *ch = a->params().channel(ids.front());
    const ScalarKey *k = ch ? ch->keyAt(frame) : nullptr;
    const Interp cur = k ? k->interp : Interp::Linear;
    const bool flat = k && std::abs(k->outSpeed) < 1e-6 && std::abs(k->inSpeed) < 1e-6;

    struct EaseItem { const char *label; Interp mode; bool in; bool out; };
    static const EaseItem items[] = {
        {"Linear",      Interp::Linear, false, false},
        {"Hold",        Interp::Hold,   false, false},
        {"Ease In",     Interp::Bezier, true,  false},
        {"Ease Out",    Interp::Bezier, false, true},
        {"Ease In/Out", Interp::Bezier, true,  true},
    };
    for (const EaseItem &e : items) {
        QAction *act = m->addAction(QString::fromUtf8(e.label));
        act->setCheckable(true);
        // A bezier key with both speeds zero is Easy Ease; the one-sided variants
        // read from the handle that is zeroed.
        bool checked = (cur == e.mode);
        if (checked && e.mode == Interp::Bezier) {
            const bool zin = k && std::abs(k->inSpeed) < 1e-6;
            const bool zout = k && std::abs(k->outSpeed) < 1e-6;
            checked = (e.in && e.out) ? flat : (e.in ? (zin && !zout) : (zout && !zin));
        }
        act->setChecked(checked);
        connect(act, &QAction::triggered, this, [this, a, frame, e] {
            doc_->applyKeyPreset(a, frame, e.mode, e.in, e.out);
            grid_->noteAnimationChanged();
            refreshContent();
            if (!curve_->isHidden()) curve_->refresh();
            emit keyChanged();
        });
    }
    m->addSeparator();
    QAction *graph = m->addAction(QStringLiteral("Open easing graph"));
    connect(graph, &QAction::triggered, this, [this, a] {
        if (curve_->isHidden()) graphBtn_->setChecked(true);
        curve_->setAdjustment(a);
        curve_->refresh();
    });
    m->addSeparator();
    QAction *del = m->addAction(QStringLiteral("Delete keyframe"));
    connect(del, &QAction::triggered, this, [this, a, ids, frame] {
        for (ParamId id : ids) doc_->removeParamKey(a, id, frame);
        grid_->noteAnimationChanged();
        refreshContent();
        emit keyChanged();
    });
    return m;
}

void TimelineDock::syncKeyButtons() {
    if (CameraLayer *cam = keyCamera()) {
        const bool camAnimated = cam->params().keyCount() > 0;
        if (addKeyBtn_) {
            addKeyBtn_->setEnabled(!cam->locked());
            addKeyBtn_->setToolTip(QStringLiteral("Add a camera keyframe at the playhead"));
            addKeyBtn_->setIcon(InkIcons::get(IconId::KeyPlus, InkTheme::recordArm(), 16));
            addKeyBtn_->setObjectName(QStringLiteral("RecordArm"));
        }
        if (delKeyBtn_) delKeyBtn_->setEnabled(camAnimated);
        if (prevKeyBtn_) prevKeyBtn_->setEnabled(camAnimated);
        if (nextKeyBtn_) nextKeyBtn_->setEnabled(camAnimated);
        return;
    }
    AdjustmentLayer *a = keyLayer();
    const bool has = a != nullptr || doc_->currentLayer() != nullptr;
    if (addKeyBtn_) {
        addKeyBtn_->setEnabled(has);
        addKeyBtn_->setToolTip(
            a ? QStringLiteral("Add a keyframe at the playhead on \"%1\"  (K)")
                    .arg(QString::fromStdString(a->name()))
              : QStringLiteral("Add a keyframe at the playhead on the selected layer  (K)"));
    }
    const bool animated = a && a->params().keyCount() > 0;
    if (delKeyBtn_) delKeyBtn_->setEnabled(animated);
    if (prevKeyBtn_) prevKeyBtn_->setEnabled(animated);
    if (nextKeyBtn_) nextKeyBtn_->setEnabled(animated);
    if (autoKeyBtn_) {
        // Arming Auto-key is what makes a tool drag write keys instead of pixels
        // — say so, because it changes what the transform tool does.
        autoKeyBtn_->setToolTip(
            "Auto-key: painting creates a cell, and dragging a transform with "
            "\"Record as keyframes\" on writes keyframes instead of pixels");
    }
    if (autoKeyBtn_ && autoKeyBtn_->isChecked() && a) {
        // A red rim on the key button while recording is armed: the one state
        // that changes what the tools do has to be visible at a glance.
        addKeyBtn_->setIcon(InkIcons::get(IconId::KeyPlus, InkTheme::recordArm(), 16));
        addKeyBtn_->setObjectName(QStringLiteral("RecordArm"));
    } else if (addKeyBtn_) {
        addKeyBtn_->setIcon(InkIcons::get(IconId::KeyPlus, InkTheme::dim(), 16));
        addKeyBtn_->setObjectName(QString());
    }
}

void TimelineDock::onUngroup() {
    const QString fid = grid_->selectedFolderId();
    if (fid.isEmpty()) return;
    GroupLayer *g = nullptr;
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    for (auto *f : folders)
        if (f->stableId() == fid.toStdString()) { g = f; break; }
    if (!g) return;
    if (doc_->ungroupFolder(g)) {
        grid_->clearLayerSelection();
        refreshContent();
        emit layersChanged();
        emit keyChanged();
    }
}

void TimelineDock::onRename(const QString &id) {
    if (id.startsWith(QStringLiteral("folder:"))) {
        const std::string fid = id.mid(7).toStdString();
        GroupLayer *g = nullptr;
        std::vector<GroupLayer*> folders;
        doc_->image().collectFolders(folders);
        for (auto *f : folders)
            if (f->stableId() == fid) { g = f; break; }
        if (!g) return;
        const QString n = QInputDialog::getText(this, QStringLiteral("Folder name"),
                                                QStringLiteral("Name:"), QLineEdit::Normal,
                                                QString::fromStdString(g->name()));
        if (n.isEmpty()) return;
        auto edit = doc_->editGuard();
        g->setName(n.toStdString());
        doc_->timeline().renameFolder(fid, n.toStdString());
        refreshContent();
        emit layersChanged();
        return;
    }
    if (CameraLayer *cam = doc_->findCamera(id.toStdString())) {
        const QString n = QInputDialog::getText(this, QStringLiteral("Camera name"),
                                                QStringLiteral("Name:"), QLineEdit::Normal,
                                                QString::fromStdString(cam->name()));
        if (n.isEmpty()) return;
        doc_->renameCamera(cam, n.toStdString());
        refreshContent();
        emit layersChanged();
        return;
    }
    PaintLayer *l = doc_->findTrack(id.toStdString());
    if (!l) return;
    const QString n = QInputDialog::getText(this, QStringLiteral("Layer name"),
                                            QStringLiteral("Name:"), QLineEdit::Normal,
                                            QString::fromStdString(l->name()));
    if (n.isEmpty()) return;
    auto edit = doc_->editGuard();
    l->setName(n.toStdString());
    doc_->touchLayer(l, doc_->currentFrame());
    refreshContent();
    emit layersChanged();
}

void TimelineDock::showCameraMenu(const QPoint &globalPos, CameraLayer *cam) {
    if (!cam) return;
    QMenu m;
    const QString scopeName = cam->scope() == CameraScope::Global   ? QStringLiteral("Global")
                              : cam->scope() == CameraScope::Folder ? QStringLiteral("Group")
                                                                    : QStringLiteral("Layer");
    QAction *title = m.addAction(QStringLiteral("Camera (%1) \u2014 %2")
                                     .arg(scopeName, QString::fromStdString(cam->name())));
    title->setEnabled(false);
    m.addSeparator();
    m.addAction(QStringLiteral("Key camera at the playhead"), this, [this, cam] {
        doc_->keyCameraAt(cam, doc_->currentFrame());
        refreshContent();
        emit keyChanged();
    });
    m.addAction(QStringLiteral("Remove camera keys at the playhead"), this, [this, cam] {
        if (doc_->removeCameraKeysAt(cam, doc_->currentFrame()) > 0) {
            refreshContent();
            emit keyChanged();
        }
    });
    m.addAction(QStringLiteral("Open easing graph"), this, [this, cam] {
        if (curve_->isHidden()) graphBtn_->setChecked(true);
        curve_->setCamera(cam);
        curve_->refresh();
    });
    m.addSeparator();
    QAction *visA = m.addAction(cam->visible() ? QStringLiteral("Disable camera")
                                               : QStringLiteral("Enable camera"));
    connect(visA, &QAction::triggered, this, [this, cam] {
        auto edit = doc_->editGuard();
        cam->setVisible(!cam->visible());
        doc_->touchCamera(cam);
        refreshContent();
        emit keyChanged();
    });
    QAction *lockA = m.addAction(cam->locked() ? QStringLiteral("Unlock camera")
                                               : QStringLiteral("Lock camera"));
    connect(lockA, &QAction::triggered, this, [this, cam] {
        auto edit = doc_->editGuard();
        cam->setLocked(!cam->locked());
        refresh();
        emit keyChanged();
    });
    m.addSeparator();
    m.addAction(QStringLiteral("Rename camera\u2026"), this, [this, cam] {
        onRename(QString::fromStdString(cam->stableId()));
    });
    m.addSeparator();
    m.addAction(QStringLiteral("Delete camera"), this, [this, cam] {
        if (doc_->deleteCamera(cam)) {
            refreshContent();
            emit layersChanged();
            emit keyChanged();
        }
    });
    m.exec(globalPos);
}

void TimelineDock::tick() {
    // The easing graph shows the segment under the PLAYHEAD and the transform
    // panel reads values AT it, so both are stale the moment it moves. Frame
    // changes arrive from many places (transport, scrub, key jumps, cell menus,
    // playback), so rather than hook each one, poll the one number that matters
    // here: an int compare per 16ms, and no path can be forgotten.
    const int now = doc_->currentFrame();
    if (now != lastFollowFrame_) {
        lastFollowFrame_ = now;
        if (!curve_->isHidden()) curve_->followPlayhead();
    }
    if (!doc_->playback().isPlaying()) return;
    auto [f, changed] = doc_->playback().advance(0.016);
    if (changed) {
        doc_->timeline().setCurrentFrame(f);
        if (follow_) grid_->ensureFrameVisible(f);
        grid_->refresh();
        syncBars();
        updateTimecode();
        updateStatus();
        emit frameChanged(f);
        if (parentWidget()) parentWidget()->update();
    }
    if (!doc_->playback().isPlaying()) syncTransport();
}

// ------------------------------------------------------------------- menus

void TimelineDock::showTrackMenu(const QPoint &globalPos, const QString &id) {
    QMenu m;
    if (id.startsWith(QStringLiteral("folder:"))) {
        const std::string fid = id.mid(7).toStdString();
        auto *f = doc_->timeline().folder(fid);
        GroupLayer *g = nullptr;
        std::vector<GroupLayer*> folders;
        doc_->image().collectFolders(folders);
        for (auto *cand : folders)
            if (cand->stableId() == fid) { g = cand; break; }
        if (!g) return;
        m.addAction(QStringLiteral("New layer in this folder"), this, [this, g] {
            doc_->setCurrentFolder(g);
            onAddLayer();
        });
        m.addAction(QStringLiteral("Add Camera for Group"), this, [this, g] {
            if (CameraLayer *cam = doc_->createCameraForFolder(g, {})) {
                doc_->keyCameraAt(cam, doc_->currentFrame());
                grid_->setSelectedLayers({QString::fromStdString(cam->stableId())});
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
        m.addAction(QStringLiteral("Add Global Camera"), this, [this] {
            if (CameraLayer *cam = doc_->createGlobalCamera({})) {
                doc_->keyCameraAt(cam, doc_->currentFrame());
                grid_->setSelectedLayers({QString::fromStdString(cam->stableId())});
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
        m.addAction(QStringLiteral("Select every layer in this folder"), this, [this, g] {
            QSet<QString> ids;
            for (auto &c : g->children())
                if (c->type() == LayerType::Paint) ids.insert(QString::fromStdString(c->stableId()));
            grid_->setSelectedLayers(ids);
            emit keyChanged();
        });
        m.addSeparator();
        m.addAction(QStringLiteral("Rename folder…"), this, [this, id] { onRename(id); });
        QAction *visA = m.addAction(g->visible() ? QStringLiteral("Hide folder") : QStringLiteral("Show folder"));
        connect(visA, &QAction::triggered, this, [this, g] {
            auto edit = doc_->editGuard();
            g->setVisible(!g->visible());
            doc_->frameCache().invalidateAll();
            refreshContent();
            emit keyChanged();
        });
        QAction *lockA = m.addAction(g->locked() ? QStringLiteral("Unlock folder") : QStringLiteral("Lock folder"));
        connect(lockA, &QAction::triggered, this, [this, g] {
            auto edit = doc_->editGuard();
            g->setLocked(!g->locked());
            refresh();
            emit keyChanged();
        });
        m.addAction(f && f->expanded ? QStringLiteral("Collapse") : QStringLiteral("Expand"), this, [this, fid] {
            if (auto *tf = doc_->timeline().folder(fid))
                doc_->timeline().setFolderExpanded(fid, !tf->expanded);
            refresh();
        });
        m.addSeparator();
        QAction *ug = m.addAction(QStringLiteral("Ungroup folder (keep layers)"));
        ug->setEnabled(folders.size() > 1);
        ug->setToolTip(folders.size() > 1 ? QString() : QStringLiteral("Needs another folder to merge into"));
        connect(ug, &QAction::triggered, this, [this, g] {
            if (doc_->ungroupFolder(g)) {
                grid_->clearLayerSelection();
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
        const bool lastFolder = folders.size() <= 1;
        const int childCount = (int)g->children().size();
        QAction *del = m.addAction(lastFolder && childCount == 0
                                       ? QStringLiteral("Delete folder")
                                       : QStringLiteral("Delete folder and its %1 layer%2")
                                             .arg(childCount)
                                             .arg(childCount == 1 ? QString() : QStringLiteral("s")));
        del->setEnabled(!(lastFolder && childCount == 0));
        del->setToolTip(lastFolder
                            ? QStringLiteral("The last folder stays (contents are removed)")
                            : QStringLiteral("Removes the folder with everything inside it"));
        connect(del, &QAction::triggered, this, [this, g] {
            if (doc_->deleteFolder(g)) {
                grid_->clearLayerSelection();
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
        m.exec(globalPos);
        return;
    }

    if (CameraLayer *camMenu = doc_->findCamera(id.toStdString())) {
        showCameraMenu(globalPos, camMenu);
        return;
    }
    PaintLayer *l = doc_->findTrack(id.toStdString());
    if (!l) return;
    doc_->setCurrentLayer(l);
    GroupLayer *owner = doc_->image().findFolderOf(l);
    if (owner) doc_->setCurrentFolder(owner);

    // Same rule as the toolbar button: on a free frame it drops a cell at the
    // playhead, on a cell it inserts one right after it (never a dead click).
    m.addAction(QStringLiteral("New cell at the playhead"), this, [this] { onNewCell(); });
    m.addAction(QStringLiteral("New layer above"), this, [this, l] {
        doc_->setCurrentLayer(l);
        if (auto *g = doc_->image().findFolderOf(l)) doc_->setCurrentFolder(g);
        onAddLayer();
    });
    m.addAction(QStringLiteral("Add Camera Layer"), this, [this, l] {
        if (CameraLayer *cam = doc_->createCameraForLayer(l, {})) {
            doc_->keyCameraAt(cam, doc_->currentFrame());
            grid_->setSelectedLayers({QString::fromStdString(cam->stableId())});
            refreshContent();
            emit layersChanged();
            emit keyChanged();
        }
    });
    m.addSeparator();

    // --- structure: grouping, ordering, deletion ---
    const int sel = grid_->selectedLayers().size();
    QAction *grp = m.addAction(sel > 1 ? QStringLiteral("Group %1 selected layers into a folder…").arg(sel)
                                       : QStringLiteral("Group selected layers into a folder…"));
    grp->setEnabled(sel > 0);
    connect(grp, &QAction::triggered, this, &TimelineDock::onGroupSelected);
    if (owner) {
        QAction *ug = m.addAction(QStringLiteral("Ungroup “%1” (keep layers)").arg(QString::fromStdString(owner->name())));
        const bool canUngroup = [&] {
            std::vector<GroupLayer*> fs;
            doc_->image().collectFolders(fs);
            return fs.size() > 1;
        }();
        ug->setEnabled(canUngroup);
        connect(ug, &QAction::triggered, this, [this, owner] {
            if (doc_->ungroupFolder(owner)) {
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
    }
    auto *moveMenu = m.addMenu(QStringLiteral("Move layer to folder"));
    std::vector<GroupLayer*> folders;
    doc_->image().collectFolders(folders);
    for (auto *f : folders) {
        QAction *a = moveMenu->addAction(QString::fromStdString(f->name()));
        a->setEnabled(f != owner);
        connect(a, &QAction::triggered, this, [this, l, f] {
            if (doc_->moveLayerToFolder(l, f)) {
                refreshContent();
                emit layersChanged();
                emit keyChanged();
            }
        });
    }
    QMenu *orderMenu = m.addMenu(QStringLiteral("Stack order"));
    // Moving past either end is a silent no-op in the model: grey the entry out
    // and say so, instead of letting the click do nothing.
    GroupLayer *orderFolder = doc_->image().findFolderOf(l);
    int stackIdx = -1, stackCount = 0;
    if (orderFolder) {
        stackCount = (int)orderFolder->children().size();
        for (int i = 0; i < stackCount; ++i)
            if (orderFolder->children()[(size_t)i].get() == static_cast<Layer *>(l)) { stackIdx = i; break; }
    }
    QAction *upA = orderMenu->addAction(QStringLiteral("Move layer up"), this, [this, l] {
        if (doc_->moveLayerInFolder(l, -1)) { refreshContent(); emit layersChanged(); emit keyChanged(); }
    });
    QAction *downA = orderMenu->addAction(QStringLiteral("Move layer down"), this, [this, l] {
        if (doc_->moveLayerInFolder(l, 1)) { refreshContent(); emit layersChanged(); emit keyChanged(); }
    });
    if (stackIdx <= 0) {
        upA->setEnabled(false);
        upA->setToolTip(QStringLiteral("Already the top row of its folder"));
    }
    if (stackIdx < 0 || stackIdx >= stackCount - 1) {
        downA->setEnabled(false);
        downA->setToolTip(QStringLiteral("Already the bottom row of its folder"));
    }

    m.addSeparator();
    // Keyframes live on the layer itself now; this is the entry someone
    // hunting for "how do I animate this" will find.
    m.addAction(QStringLiteral("Key this layer at the playhead"), this, [this, l] {
        doc_->setCurrentLayer(l);
        onAddKey();
    });
    m.addSeparator();
    m.addAction(QStringLiteral("Rename layer…"), this, [this, id] { onRename(id); });
    m.addAction(QStringLiteral("Duplicate layer"), this, [this, l] {
        doc_->duplicateTrack(l);
        refreshContent();
        emit layersChanged();
        emit keyChanged();
    });
    m.addSeparator();
    QAction *visA = m.addAction(l->visible() ? QStringLiteral("Hide layer") : QStringLiteral("Show layer"));
    connect(visA, &QAction::triggered, this, [this, l] {
        auto edit = doc_->editGuard();
        l->setVisible(!l->visible());
        doc_->frameCache().invalidateAll();
        refreshContent();
        emit keyChanged();
    });
    QAction *lockA = m.addAction(l->locked() ? QStringLiteral("Unlock layer") : QStringLiteral("Lock layer"));
    connect(lockA, &QAction::triggered, this, [this, l] {
        auto edit = doc_->editGuard();
        l->setLocked(!l->locked());
        refresh();
        emit keyChanged();
    });
    auto *opMenu = m.addMenu(QStringLiteral("Opacity"));
    for (int pct : {25, 50, 75, 100}) {
        QAction *a = opMenu->addAction(QString::number(pct) + QStringLiteral("%"));
        a->setCheckable(true);
        a->setChecked(int(l->opacity() * 100) == pct);
        connect(a, &QAction::triggered, this, [this, l, pct] {
            auto edit = doc_->editGuard();
            l->setOpacity(pct / 100.0f);
            doc_->frameCache().invalidateAll();
            refresh();
            emit keyChanged();
        });
    }
    auto *blMenu = m.addMenu(QStringLiteral("Blend mode"));
    struct BM { const char *n; BlendMode b; };
    for (auto bm : {BM{"Normal", BlendMode::Normal}, BM{"Multiply", BlendMode::Multiply},
                    BM{"Screen", BlendMode::Screen}, BM{"Erase", BlendMode::Erase}}) {
        QAction *a = blMenu->addAction(QString::fromLatin1(bm.n));
        a->setCheckable(true);
        a->setChecked(l->blend() == bm.b);
        connect(a, &QAction::triggered, this, [this, l, b = bm.b] {
            auto edit = doc_->editGuard();
            l->setBlend(b);
            doc_->frameCache().invalidateAll();
            refresh();
            emit keyChanged();
        });
    }
    auto *colMenu = m.addMenu(QStringLiteral("Layer color"));
    QAction *autoC = colMenu->addAction(QStringLiteral("Folder default"));
    connect(autoC, &QAction::triggered, this, [this, l] {
        if (auto *r = doc_->timeline().row(l->stableId())) r->colorOverride = -1;
        refresh();
        emit keyChanged();
    });
    for (int i = 0; i < 8; ++i) {
        QAction *a = colMenu->addAction(QStringLiteral("Hue %1").arg(i + 1));
        QPixmap sw(14, 14);
        sw.fill(InkTheme::layerHue(i));
        a->setIcon(QIcon(sw));
        connect(a, &QAction::triggered, this, [this, l, i] {
            if (auto *r = doc_->timeline().row(l->stableId())) r->colorOverride = i;
            refresh();
            emit keyChanged();
        });
    }
    QAction *thumbA = m.addAction(QStringLiteral("Cel thumbnails"));
    thumbA->setCheckable(true);
    thumbA->setChecked(doc_->timeline().row(l->stableId()) && doc_->timeline().row(l->stableId())->showThumbs);
    connect(thumbA, &QAction::triggered, this, [this, l](bool on) {
        if (auto *r = doc_->timeline().row(l->stableId())) r->showThumbs = on;
        refreshContent();
        emit keyChanged();
    });
    m.addSeparator();
    m.addAction(QStringLiteral("Select all cells on layer"), this, [this, l] {
        QSet<TimelineSel> s;
        const QString tid = QString::fromStdString(l->stableId());
        for (const auto &c : doc_->timeline().cells(l->stableId()))
            s.insert({tid, c.start});
        grid_->setSelection(s);
        emit keyChanged();
    });
    QAction *clearA = m.addAction(QStringLiteral("Clear all cells on layer"), this, [this, l] {
        doc_->removeKeys(l, l->keys(), false);
        refreshContent();
        emit keyChanged();
    });
    clearA->setEnabled(!l->keys().empty() && !l->locked());
    if (l->locked()) clearA->setToolTip(QStringLiteral("Layer is locked"));
    else if (l->keys().empty()) clearA->setToolTip(QStringLiteral("Layer has no cells"));
    m.addAction(QStringLiteral("Delete layer"), this, [this, l] {
        doc_->deleteTrack(l);
        refreshContent();
        emit layersChanged();
        emit keyChanged();
    });
    m.exec(globalPos);
}

void TimelineDock::showLaneMenu(const QPoint &globalPos, const QString &trackId, int frame) {
    PaintLayer *l = doc_->findTrack(trackId.toStdString());
    if (!l) return;
    doc_->setCurrentLayer(l);
    const int covering = doc_->cellStartAt(l, frame);
    const bool locked = l->locked();
    // Cell edits refuse to run on a locked layer: say so instead of letting the
    // click land on nothing.
    const QString lockTip = locked ? QStringLiteral("Layer is locked") : QString();
    auto cellOp = [&](QAction *a) {
        a->setEnabled(!locked);
        if (locked) a->setToolTip(lockTip);
        return a;
    };
    const auto sel = grid_->selection();
    QMenu m;

    if (covering >= 0) {
        const int len = doc_->cellLength(l, covering);
        QAction *hdr = m.addAction(QStringLiteral("Cell %1 · frames %2–%3")
                                       .arg(doc_->timeline().cellIndexAt(trackId.toStdString(), frame) + 1)
                                       .arg(covering).arg(doc_->cellEnd(l, covering)));
        hdr->setEnabled(false);
        m.addSeparator();
        auto *expMenu = m.addMenu(QStringLiteral("Exposure"));
        if (locked) expMenu->setEnabled(false);
        struct Len { const char *label; int value; };
        for (Len e : {Len{"1 frame", 1}, Len{"2 frames", 2}, Len{"3 frames", 3},
                      Len{"6 frames", 6}, Len{"12 frames", 12}}) {
            QAction *a = expMenu->addAction(QString::fromLatin1(e.label));
            a->setCheckable(true);
            a->setChecked(len == e.value);
            connect(a, &QAction::triggered, this, [this, l, covering, e] {
                doc_->setCellLength(l, covering, e.value);
                refreshContent();
                emit keyChanged();
            });
        }
        expMenu->addSeparator();
        expMenu->addAction(QStringLiteral("To next cell"), this, [this, l, covering] {
            doc_->extendCellToNext(l, covering);
            refreshContent();
            emit keyChanged();
        });
        expMenu->addAction(QStringLiteral("To the end"), this, [this, l, covering] {
            doc_->extendCellToEnd(l, covering);
            refreshContent();
            emit keyChanged();
        });
        cellOp(m.addAction(QStringLiteral("Longer  ]"), this, [this, l, covering] {
            doc_->setCellEnd(l, covering, doc_->cellEnd(l, covering) + 1);
            refreshContent();
            emit keyChanged();
        }));
        cellOp(m.addAction(QStringLiteral("Shorter  ["), this, [this, l, covering] {
            doc_->setCellEnd(l, covering, doc_->cellEnd(l, covering) - 1);
            refreshContent();
            emit keyChanged();
        }));
        m.addSeparator();
    }

    if (covering < 0) {
        cellOp(m.addAction(QStringLiteral("New cell here  (N)"), this, [this, l, frame] {
            if (doc_->addBlankKey(l, frame)) { refreshContent(); emit keyChanged(); }
        }));
    } else {
        // A new cell always fits between two cells: at the clicked cell's start
        // (before it) or right after its end (after it), later cells rippling right.
        cellOp(m.addAction(QStringLiteral("Insert cell before  (ripples this layer)"), this,
                           [this, l, covering] {
            if (doc_->insertCell(l, covering)) {
                doc_->setCurrentFrame(covering);
                refreshContent();
                emit keyChanged();
            }
        }));
        const int after = doc_->cellEnd(l, covering) + 1;
        cellOp(m.addAction(QStringLiteral("Insert cell after  (ripples this layer)"), this,
                           [this, l, after] {
            if (doc_->insertCell(l, after)) {
                doc_->setCurrentFrame(after);
                refreshContent();
                emit keyChanged();
            }
        }));
    }
    // "Active cell" is the one at the playhead; the clicked frame is the
    // DESTINATION. (Using the clicked cell as its own source made this a no-op.)
    const int activeStart = doc_->cellStartAt(l, doc_->currentFrame());
    const int activeEnd = activeStart >= 0 ? doc_->cellEnd(l, activeStart) : -1;
    const bool frameInActive = activeStart >= 0 && frame >= activeStart && frame <= activeEnd;
    QAction *dupHere = cellOp(m.addAction(QStringLiteral("Duplicate the active cell here  (D)"), this,
                                          [this, l, frame] {
        int src = doc_->cellStartAt(l, doc_->currentFrame());
        if (src < 0) src = doc_->cellStartAt(l, frame);
        if (src < 0 || src == frame) return;
        if (doc_->duplicateKey(l, src, frame, /*overwrite=*/true)) {
            doc_->setCurrentFrame(frame);
            refreshContent();
            emit keyChanged();
        }
    }));
    if (activeStart < 0) {
        dupHere->setEnabled(false);
        dupHere->setToolTip(QStringLiteral("Put the playhead on a cell to copy it here"));
    } else if (frameInActive) {
        dupHere->setEnabled(false);
        dupHere->setToolTip(QStringLiteral("That frame is already inside the active cell"));
    }
    m.addSeparator();
    QAction *copyA = m.addAction(QStringLiteral("Copy cells  (Ctrl+C)"), this, [this] {
        auto s = grid_->selection();
        if (s.isEmpty()) return;
        std::map<QString, std::vector<int>> byT;
        for (auto &e : s) byT[e.trackId].push_back(e.frame);
        for (auto &[tid, fr] : byT)
            if (PaintLayer *t = doc_->findTrack(tid.toStdString())) { doc_->copyKeys(t, fr); break; }
    });
    copyA->setEnabled(!sel.isEmpty());
    if (sel.isEmpty()) copyA->setToolTip(QStringLiteral("Select a cell first"));
    QAction *pasteA = cellOp(m.addAction(QStringLiteral("Paste at playhead  (Ctrl+V)"), this, [this, l] {
        if (doc_->pasteKeys(l, doc_->currentFrame(), false) > 0) {
            refreshContent();
            emit keyChanged();
        }
    }));
    if (!doc_->clipboard().valid) {
        pasteA->setEnabled(false);
        pasteA->setToolTip(QStringLiteral("Copy a cell first"));
    }
    int selectedOnThisLayer = 0;
    for (const auto &e : sel)
        if (e.trackId == trackId) ++selectedOnThisLayer;
    QAction *mirrorA = cellOp(m.addAction(QStringLiteral("Mirror selected  (M)"), this, [this] {
        auto s = grid_->selection();
        std::map<QString, std::vector<int>> byT;
        for (auto &e : s) byT[e.trackId].push_back(e.frame);
        for (auto &[tid, fr] : byT)
            if (PaintLayer *t = doc_->findTrack(tid.toStdString())) {
                auto lo = *std::min_element(fr.begin(), fr.end());
                auto hi = *std::max_element(fr.begin(), fr.end());
                doc_->mirrorKeys(t, lo, hi);
            }
        refreshContent();
        emit keyChanged();
    }));
    if (selectedOnThisLayer < 2) {
        mirrorA->setEnabled(false);
        mirrorA->setToolTip(QStringLiteral("Select at least two cells first "
                                           "(Ctrl-click, or drag across the lane)"));
    }
    QAction *delCell = m.addAction(QStringLiteral("Delete cell  (Del)"));
    delCell->setEnabled(covering >= 0 && !locked);
    if (locked) delCell->setToolTip(lockTip);
    connect(delCell, &QAction::triggered, this, [this, l, frame] {
        const int start = doc_->cellStartAt(l, frame);
        if (start >= 0) {
            doc_->removeKeys(l, {start}, false);
            refreshContent();
            emit keyChanged();
        }
    });
    QAction *delPull = m.addAction(QStringLiteral("Delete cell and close the gap"));
    delPull->setEnabled(covering >= 0 && !locked);
    if (locked) delPull->setToolTip(lockTip);
    connect(delPull, &QAction::triggered, this, [this, l, frame] {
        const int start = doc_->cellStartAt(l, frame);
        if (start >= 0) {
            doc_->removeKeys(l, {start}, true);
            refreshContent();
            emit keyChanged();
        }
    });
    m.addSeparator();
    cellOp(m.addAction(QStringLiteral("Insert one frame (ripples this layer)"), this, [this, l, frame] {
        doc_->shiftKeys(l, frame, 1);
        refreshContent();
        emit keyChanged();
    }));
    cellOp(m.addAction(QStringLiteral("Remove one frame (ripples this layer)"), this, [this, l, frame] {
        doc_->shiftKeys(l, frame, -1);
        refreshContent();
        emit keyChanged();
    }));
    m.addSeparator();
    m.addAction(QStringLiteral("Select all cells on this layer"), this, [this, l] {
        QSet<TimelineSel> s;
        const QString tid = QString::fromStdString(l->stableId());
        for (const auto &c : doc_->timeline().cells(l->stableId())) s.insert({tid, c.start});
        grid_->setSelection(s);
        emit keyChanged();
    });
    QAction *lockHere = m.addAction(l->locked() ? QStringLiteral("Unlock layer")
                                                : QStringLiteral("Lock layer"),
                                   this, [this, l] {
        l->setLocked(!l->locked());
        refresh();
        emit keyChanged();
    });
    lockHere->setCheckable(true);
    lockHere->setChecked(l->locked());
    m.addSeparator();
    m.addAction(QStringLiteral("Go to frame %1").arg(frame), this, [this, frame] {
        doc_->setCurrentFrame(frame);
        if (follow_) grid_->ensureFrameVisible(frame);
        refresh();
        emit frameChanged(frame);
    });
    m.exec(globalPos);
}

void TimelineDock::showRulerMenu(const QPoint &globalPos) {
    QMenu m;
    const int f = doc_->currentFrame();
    m.addAction(QStringLiteral("Set range in to the playhead  (I)"), this, [this, f] {
        grid_->setRange(f, std::max(f, grid_->rangeOut()));
        onGridRange(grid_->rangeIn(), grid_->rangeOut());
    });
    m.addAction(QStringLiteral("Set range out to the playhead  (O)"), this, [this, f] {
        grid_->setRange(std::min(f, grid_->rangeIn()), f);
        onGridRange(grid_->rangeIn(), grid_->rangeOut());
    });
    m.addSeparator();
    m.addAction(QStringLiteral("Reset range to the whole project"), this, [this] {
        const int last = doc_->timeline().frameCount() - 1;
        grid_->setRange(0, last);
        onGridRange(0, last);
    });
    m.addSeparator();
    QAction *cellRange = m.addAction(QStringLiteral("Range around the current cell"), this, [this] {
        if (PaintLayer *l = doc_->currentLayer()) {
            const int start = doc_->cellStartAt(l, doc_->currentFrame());
            if (start < 0) return;
            const int end = doc_->cellEnd(l, start);
            grid_->setRange(start, end);
            onGridRange(start, end);
        }
    });
    {
        PaintLayer *l = doc_->currentLayer();
        const bool hasCell = l && doc_->cellStartAt(l, doc_->currentFrame()) >= 0;
        cellRange->setEnabled(hasCell);
        if (!hasCell)
            cellRange->setToolTip(QStringLiteral("No cell at the playhead on the current layer"));
    }
    m.exec(globalPos);
}

} // namespace ink
