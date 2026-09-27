#include "ui/tools/InkMoveTool.h"
#include "core/InkSelection.h"
#include "ui/InkDocument.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QStringList>
#include <QToolButton>
#include <cmath>

namespace ink {

void MoveTool::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    ctx_ = &ctx;
    resetState();
}

void MoveTool::deactivate() { cancelPrimary(); }

QString MoveTool::statusHint() const {
    QString base = QStringLiteral(
        "Drag to move the selection · Alt copies · Shift constrains · arrows nudge");
    // Opt-in discoverability, same contract as the transform tool: a whole-layer
    // drag on an animated layer bakes pixels unless RECORD is on, so say so.
    if (!recording_ && !selectionScope_ && !recordKeyframes_ && ctx_ && ctx_->doc() &&
        ctx_->doc()->autoKeyEnabled()) {
        if (const AdjustmentLayer *a = ctx_->recordTarget())
            if (a->params().keyCount() > 0)
                base += QStringLiteral(" · layer has keys — enable RECORD to animate it");
    }
    return base;
}

void MoveTool::resetState() {
    lifted_ = FloatRegion{};
    eraseMask_ = Selection{};
    memento_ = TileMemento{};
    device_ = nullptr;
    area_ = Rect{};
    dragging_ = false;
    copying_ = false;
    liftedOnce_ = false;
    selectionScope_ = false;
    recording_ = false;
    recordFrame_ = -1;
    freshX_ = freshY_ = false;
    offsetX_ = offsetY_ = 0;
}

// The pose this session measures against. Derived from the model, never from
// the tool's own state, and re-read at every press so a key dragged in the
// timeline under an open tool cannot leave the tool aiming at a stale frame.
void MoveTool::refreshDisplayMapping() {
    Mat3 fwd = Mat3::identity();
    if (ctx_ && ctx_->doc()) {
        if (PaintLayer *pl = ctx_->doc()->currentLayer()) {
            if (!pl->params().empty()) {
                const Rect canvas = documentRect();
                fwd = pl->params().matrixFor(pl->params().frameOr(canvas),
                                            double(ctx_->doc()->currentFrame()), nullptr);
            }
        }
    }
    bool invertible = false;
    Mat3 inv = fwd.inverted(&invertible);
    if (!invertible) { // degenerate pose: no mapping beats a singular one
        fwd = Mat3::identity();
        inv = Mat3::identity();
    }
    stateToDisplay_ = fwd;
    displayToState_ = inv;
}

Rect MoveTool::documentRect() const {
    const Image &img = ctx_->doc()->image();
    return {0, 0, img.width(), img.height()};
}

const Selection *MoveTool::mask() const { return ctx_->selectionClip(); }

QString MoveTool::commandName() const {
    return hadSelection_ ? QStringLiteral("Move selection") : QStringLiteral("Move layer");
}

// Grabs the pixels to move. `useSelection` picks the scope: the selection
// contents, or the whole layer when the drag started outside any selection
// (the Krita/Photoshop rule). Returns false when there is nothing to move.
bool MoveTool::grab(bool useSelection) {
    resetState();
    device_ = ctx_->editableDevice();
    if (!device_) {
        ctx_->setStatus(ctx_->blockedHint());
        return false;
    }
    selectionScope_ = useSelection && !ctx_->selection().isEmpty();
    hadSelection_ = selectionScope_;
    const Rect doc = documentRect();
    area_ = selectionScope_ ? intersectRect(ctx_->selection().bounds(), doc) : doc;
    if (area_.isEmpty())
        return false;
    lifted_ = liftRegion(*device_, area_, selectionScope_ ? mask() : nullptr);
    if (lifted_.empty())
        return false;
    if (selectionScope_) {
        // Snapshot the mask: the live one is translated every drag frame, but
        // the source pixels have to be erased where they still are.
        eraseMask_ = ctx_->selection();
    }
    return true;
}

// `at` arrives in DISPLAY space; the mask is stored in DEVICE space, so the
// hit test has to happen where the mask actually is.
bool MoveTool::grabAt(const Vec2 &displayAt) {
    const Vec2 at = toDevice(displayAt);
    const Selection *m = ctx_->selectionClip();
    const bool inside = m && m->contains(int(std::floor(at.x)), int(std::floor(at.y)));
    return grab(inside);
}

// --- keyframe recording ------------------------------------------------------

// Opt-in: Auto-key alone must not hijack the drag (it defaults ON).
void MoveTool::setRecordKeyframes(bool on) {
    if (recordKeyframes_ == on)
        return;
    recordKeyframes_ = on;
    if (!ctx_)
        return;
    // Settle an in-flight drag into its new mode, exactly like the transform
    // tool's toggle does — one code path for UI and programmatic changes.
    if (dragging_)
        endPrimary(ToolEvent{});
}

bool MoveTool::canRecord() const {
    if (!ctx_ || !ctx_->recording() || selectionScope_)
        return false;
    // Strictly OPT-IN, like the transform tool: an animated layer must not
    // hijack the drag into key-writing unasked (the "move tool broken after
    // keying" regression). Toggle off = pixels move, toggle on = keys.
    return recordKeyframes_;
}

bool MoveTool::beginRecord(const Vec2 &at) {
    if (!canRecord())
        return false;
    start_ = at;
    dragging_ = true;
    recording_ = true;
    recordFrame_ = ctx_->doc()->currentFrame();
    // Start from the pose the animation already holds, so a second drag at a
    // later frame continues the move instead of restarting it from zero.
    baseX_ = ctx_->recordedValue(ParamId::PositionX, recordFrame_);
    baseY_ = ctx_->recordedValue(ParamId::PositionY, recordFrame_);
    if (const AdjustmentLayer *a = ctx_->recordTarget()) {
        freshX_ = !a->params().isAnimated(ParamId::PositionX);
        freshY_ = !a->params().isAnimated(ParamId::PositionY);
    } else {
        freshX_ = freshY_ = false;
    }
    ctx_->beginParamRecord(QStringLiteral("Move"), recordFrame_);
    // No key is written yet: a click that never moves must not leave one behind.
    // endParamRecord() drops an unchanged record on the floor.
    ctx_->setStatus(QStringLiteral("Recording the move · Esc reverts the drag"));
    return true;
}

void MoveTool::recordOffset(double dx, double dy) {
    ctx_->recordParam(ParamId::PositionX, baseX_ + dx, recordFrame_);
    ctx_->recordParam(ParamId::PositionY, baseY_ + dy, recordFrame_);
    ctx_->requestRebuild();
}

void MoveTool::closeRecordWithHint() {
    const int frame = recordFrame_;
    const bool wasFreshX = freshX_, wasFreshY = freshY_;
    ctx_->endParamRecord();
    if (!ctx_ || !(wasFreshX || wasFreshY))
        return;
    if (const AdjustmentLayer *a = ctx_->recordTarget()) {
        const ParamBlock &b = a->params();
        QStringList first;
        if (wasFreshX && b.isAnimated(ParamId::PositionX))
            first << QStringLiteral("Position X");
        if (wasFreshY && b.isAnimated(ParamId::PositionY))
            first << QStringLiteral("Position Y");
        if (!first.isEmpty())
            ctx_->setStatus(
                QStringLiteral("Keyframes recorded — pixels untouched · first %1 key at frame %2 — Add-key an earlier frame to animate from there")
                    .arg(first.join(QStringLiteral(", ")))
                    .arg(frame));
    }
}

void MoveTool::beginPrimary(const ToolEvent &e) {
    if (!ctx_ || dragging_)
        return;
    // Re-read the pose every press: the animation can have moved under the
    // tool (a key dragged in the timeline) without the playhead changing.
    refreshDisplayMapping();
    // One space from here on: the lift, the erase, the stamp and the recorded
    // Position deltas are all DEVICE space, so the press is captured there too.
    const Vec2 at = toDevice(e.docPos);
    if (!grabAt(e.docPos)) {
        // Nothing to lift is not the same as nothing to animate: a blank cel can
        // still carry keyframes.
        beginRecord(at);
        return;
    }
    if (canRecord()) {
        lifted_ = FloatRegion{}; // the pixels stay where they are
        memento_ = TileMemento{};
        beginRecord(at);
        return;
    }
    copying_ = (e.mods & Qt::AltModifier) != 0;
    start_ = at;
    dragging_ = true;
    if (copying_) {
        ctx_->setStatus(QStringLiteral("Copying the selection · release to drop"));
        ctx_->requestRepaint();
    } else {
        applyOffset(0, 0); // lift: the source area clears right away
        ctx_->setStatus(QStringLiteral("Moving the selection · Esc cancels"));
    }
}

void MoveTool::continuePrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    const Vec2 p = toDevice(e.docPos);
    double dx = std::lround(p.x - start_.x);
    double dy = std::lround(p.y - start_.y);
    if (e.mods & Qt::ShiftModifier) {
        if (std::abs(dx) >= std::abs(dy))
            dy = 0;
        else
            dx = 0;
    }
    if (recording_) {
        if (int(dx) == offsetX_ && int(dy) == offsetY_)
            return;
        offsetX_ = int(dx);
        offsetY_ = int(dy);
        recordOffset(dx, dy);
        return;
    }
    if (int(dx) == offsetX_ && int(dy) == offsetY_)
        return;
    applyOffset(int(dx), int(dy));
}

void MoveTool::applyOffset(int dx, int dy) {
    if (!device_ || lifted_.empty())
        return;
    // Every frame rebuilds the move from the pre-drag state, so intermediate
    // stamps can never smear into each other; the memento is re-captured
    // against those restored tiles.
    if (!memento_.empty())
        device_->store().restore(memento_);
    memento_ = TileMemento{};
    Rect dirty;
    if (!copying_)
        dirty = eraseRegion(*device_, area_, selectionScope_ ? &eraseMask_ : nullptr, &memento_);
    dirty = uniteRect(dirty, stampRegion(*device_, lifted_, dx, dy, documentRect(), &memento_));
    const int stepX = dx - offsetX_, stepY = dy - offsetY_;
    if (selectionScope_ && (stepX || stepY)) {
        ctx_->selection().translate(stepX, stepY);
        ctx_->notifySelectionChanged();
    }
    offsetX_ = dx;
    offsetY_ = dy;
    liftedOnce_ = true;
    if (!dirty.isEmpty())
        ctx_->markDirty(dirty);
    ctx_->requestRepaint();
}

void MoveTool::endPrimary(const ToolEvent &e) {
    Q_UNUSED(e);
    if (!dragging_)
        return;
    dragging_ = false;
    if (recording_) {
        // One undo step for the whole drag; an unmoved click pushes nothing.
        closeRecordWithHint();
        resetState();
        return;
    }
    if (liftedOnce_ && device_ && !memento_.empty()) {
        ctx_->selection().invalidateOutline();
        ctx_->pushStrokeCommand(commandName(), device_, std::move(memento_));
        ctx_->setStatus(QStringLiteral("Moved %1 × %2 px").arg(area_.w).arg(area_.h));
    }
    resetState();
}

void MoveTool::cancelPrimary() {
    if (!dragging_)
        return;
    dragging_ = false;
    if (recording_) {
        // Esc abandons the recorded keys: no undo entry, no pixel change.
        ctx_->doc()->cancelParamRecord();
        resetState();
        ctx_->requestRebuild();
        return;
    }
    if (device_ && !memento_.empty())
        device_->store().restore(memento_);
    if (offsetX_ || offsetY_)
        ctx_->selection().translate(-offsetX_, -offsetY_);
    ctx_->selection().invalidateOutline();
    resetState();
    ctx_->notifySelectionChanged();
    // The drag may have warmed the frame cache with the moved pixels: drop it,
    // or the rebuild would serve them back after a revert.
    if (ctx_->doc())
        ctx_->doc()->frameCache().invalidate(ctx_->doc()->currentFrame());
    ctx_->requestRebuild();
}

bool MoveTool::nudge(int dx, int dy, Qt::KeyboardModifiers mods) {
    if (!ctx_ || dragging_)
        return false;
    // An arrow key moves the ink by N pixels ON SCREEN, so on a posed layer the
    // device delta has to be the one that lands there - otherwise a nudged
    // stroke drifts away from the key it was nudged with.
    refreshDisplayMapping();
    {
        const Vec2 dev = toDeviceVector({double(dx), double(dy)});
        dx = int(std::lround(dev.x));
        dy = int(std::lround(dev.y));
    }
    if (!grab(!ctx_->selection().isEmpty())) {
        // Arrows on a blank cel still nudge the animation.
        if (!beginRecord({0.0, 0.0}))
            return false;
        copying_ = false;
        recordOffset(dx, dy);
        closeRecordWithHint();
        resetState();
        return true;
    }
    if (canRecord()) {
        lifted_ = FloatRegion{};
        memento_ = TileMemento{};
        beginRecord({0.0, 0.0});
        recordOffset(dx, dy);
        closeRecordWithHint();
        resetState();
        return true;
    }
    copying_ = (mods & Qt::AltModifier) != 0;
    applyOffset(dx, dy);
    const bool moved = liftedOnce_ && !memento_.empty();
    if (moved) {
        ctx_->selection().invalidateOutline();
        ctx_->pushStrokeCommand(commandName(), device_, std::move(memento_));
    }
    resetState();
    return moved;
}

void MoveTool::flip(bool horizontal) {
    if (!ctx_ || dragging_)
        return;
    if (!grab(!ctx_->selection().isEmpty()))
        return;
    const FloatRegion mirrored = flipRegion(lifted_, horizontal);
    Rect dirty = eraseRegion(*device_, area_, selectionScope_ ? &eraseMask_ : nullptr, &memento_);
    dirty = uniteRect(dirty, stampRegion(*device_, mirrored, 0, 0, documentRect(), &memento_));
    if (selectionScope_)
        ctx_->selection().mirror(horizontal);
    ctx_->selection().invalidateOutline();
    if (!memento_.empty()) {
        ctx_->markDirty(dirty);
        ctx_->pushStrokeCommand(horizontal ? QStringLiteral("Flip horizontal")
                                           : QStringLiteral("Flip vertical"),
                                device_, std::move(memento_));
        ctx_->setStatus(horizontal ? QStringLiteral("Flipped horizontally")
                                   : QStringLiteral("Flipped vertically"));
    }
    resetState();
    ctx_->notifySelectionChanged();
}

QWidget *MoveTool::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *scope = InkOptions::value(QStringLiteral("-"), page, 96);
    auto refresh = [this, scope] {
        const Selection &sel = ctx_->selection();
        scope->setText(sel.isEmpty()
                           ? QStringLiteral("Whole layer")
                           : QStringLiteral("Selection %1 × %2").arg(sel.bounds().w).arg(sel.bounds().h));
    };
    refresh();
    connect(ctx_, &ToolContext::selectionChanged, page, [refresh] { refresh(); });
    InkOptions::addField(row, QStringLiteral("MOVE"), scope);

    row->addWidget(InkOptions::separator(page));
    auto *flipH = InkOptions::button(QStringLiteral("Flip H"), page);
    flipH->setToolTip(QStringLiteral("Mirror the selection horizontally"));
    connect(flipH, &QToolButton::clicked, page, [this] { flip(true); });
    row->addWidget(flipH);
    auto *flipV = InkOptions::button(QStringLiteral("Flip V"), page);
    flipV->setToolTip(QStringLiteral("Mirror the selection vertically"));
    connect(flipV, &QToolButton::clicked, page, [this] { flip(false); });
    row->addWidget(flipV);

    row->addWidget(InkOptions::separator(page));
    auto *record = InkOptions::toggle(QStringLiteral("Record as keyframes"), recordKeyframes_, page);
    record->setToolTip(QStringLiteral("With Auto-key on, whole-layer drags write Position "
                                      "keyframes directly on the layer"));
    connect(record, &QToolButton::toggled, page, [this](bool on) { setRecordKeyframes(on); });
    row->addWidget(record);

    row->addWidget(InkOptions::separator(page));
    auto *hint = new QLabel(QStringLiteral("Alt drag copies · Shift locks the axis · Esc cancels"), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);
    row->addStretch(1);
    return page;
}

} // namespace ink
