#include "ui/tools/InkMoveTool.h"
#include "core/InkSelection.h"
#include "ui/InkDocument.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <cmath>

namespace ink {

void MoveTool::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    ctx_ = &ctx;
    resetState();
}

void MoveTool::deactivate() { cancelPrimary(); }

void MoveTool::resetState() {
    lifted_ = FloatRegion{};
    eraseMask_ = Selection{};
    memento_ = TileMemento{};
    device_ = nullptr;
    area_ = Rect{};
    dragging_ = false;
    copying_ = false;
    liftedOnce_ = false;
    offsetX_ = offsetY_ = 0;
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
        ctx_->setStatus(QStringLiteral("Layer is locked or not paintable"));
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

bool MoveTool::grabAt(const Vec2 &at) {
    const Selection *m = ctx_->selectionClip();
    const bool inside = m && m->contains(int(std::floor(at.x)), int(std::floor(at.y)));
    return grab(inside);
}

void MoveTool::beginPrimary(const ToolEvent &e) {
    if (!ctx_ || dragging_)
        return;
    if (!grabAt(e.docPos))
        return;
    copying_ = (e.mods & Qt::AltModifier) != 0;
    start_ = e.docPos;
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
    int dx = int(std::lround(e.docPos.x - start_.x));
    int dy = int(std::lround(e.docPos.y - start_.y));
    if (e.mods & Qt::ShiftModifier) {
        if (std::abs(dx) >= std::abs(dy))
            dy = 0;
        else
            dx = 0;
    }
    if (dx == offsetX_ && dy == offsetY_)
        return;
    applyOffset(dx, dy);
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
    if (device_ && !memento_.empty())
        device_->store().restore(memento_);
    if (offsetX_ || offsetY_)
        ctx_->selection().translate(-offsetX_, -offsetY_);
    ctx_->selection().invalidateOutline();
    resetState();
    ctx_->notifySelectionChanged();
    ctx_->requestRebuild();
}

bool MoveTool::nudge(int dx, int dy, Qt::KeyboardModifiers mods) {
    if (!ctx_ || dragging_)
        return false;
    if (!grab(!ctx_->selection().isEmpty()))
        return false;
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
    auto *hint = new QLabel(QStringLiteral("Alt drag copies · Shift locks the axis · Esc cancels"), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);
    row->addStretch(1);
    return page;
}

} // namespace ink
