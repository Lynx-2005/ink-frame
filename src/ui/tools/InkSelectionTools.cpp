#include "ui/tools/InkSelectionTools.h"
#include "ui/InkDocument.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

QRectF boundsOf(const Vec2 &a, const Vec2 &b) {
    return QRectF(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                  QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));
}

} // namespace

SelectionAction SelectionToolBase::effectiveAction(Qt::KeyboardModifiers mods) const {
    if (mods & Qt::ShiftModifier)
        return SelectionAction::Add;
    if (mods & Qt::AltModifier)
        return SelectionAction::Subtract;
    if (mods & Qt::ControlModifier)
        return SelectionAction::Intersect;
    return mode_;
}

void SelectionToolBase::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    dragging_ = false;
    path_.clear();
}

void SelectionToolBase::deactivate() {
    dragging_ = false;
    path_.clear();
}

void SelectionToolBase::beginPrimary(const ToolEvent &e) {
    if (!ctx_)
        return;
    dragging_ = true;
    start_ = current_ = e.docPos;
    path_.clear();
    path_.push_back(e.docPos);
    ctx_->requestRepaint();
}

void SelectionToolBase::continuePrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    current_ = e.docPos;
    if (!path_.empty()) {
        const Vec2 &last = path_.back();
        if (std::hypot(e.docPos.x - last.x, e.docPos.y - last.y) >= 1.5)
            path_.push_back(e.docPos);
    }
    ctx_->requestRepaint();
}

void SelectionToolBase::endPrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    dragging_ = false;
    current_ = e.docPos;
    Selection &sel = ctx_->selection();
    const std::vector<Vec2> poly = polygon();
    if (poly.size() < 3) {
        // A plain click clears the selection, unless a modifier asked to
        // combine with what is already there.
        if (effectiveAction(e.mods) == SelectionAction::Replace && !sel.isEmpty()) {
            sel.clear();
            ctx_->notifySelectionChanged();
            ctx_->setStatus(QStringLiteral("Deselected"));
        }
        path_.clear();
        ctx_->requestRepaint();
        return;
    }
    // Remember whose ink this mask clips, so the compositor can keep the ants
    // on the stroke when a keyframed transform moves it (the target id is the
    // paint layer the selection was taken on).
    if (PaintLayer *pl = ctx_->doc()->currentLayer())
        sel.setTargetId(pl->id());
    // ...and re-derive the ants' display map BEFORE mapping the polygon into the
    // mask. applyPolygon() back-maps a document-space path into device space
    // only while that map is current, and the mask's map is dropped by
    // reset() (Select All, Deselect, a fresh document, the first selection of a
    // session). Syncing only afterwards - as this path used to - stored a
    // selection drawn on a keyframed layer in DISPLAY space, so the mask stopped
    // matching the ink it circled. The target id above is what the sync reads,
    // so this order is load-bearing.
    ctx_->doc()->syncSelectionTransform();
    sel.applyPolygon(poly, effectiveAction(e.mods), antialias_);
    path_.clear();
    ctx_->notifySelectionChanged();
    ctx_->requestRepaint();
    const Rect b = sel.bounds();
    ctx_->setStatus(sel.isEmpty() ? QStringLiteral("Selection cleared")
                                  : QStringLiteral("Selection %1 × %2 px").arg(b.w).arg(b.h));
}

void SelectionToolBase::cancelPrimary() {
    if (!dragging_)
        return;
    dragging_ = false;
    path_.clear();
    ctx_->requestRepaint();
}

void SelectionToolBase::paintOverlay(QPainter &p, const OverlayInfo &info) const {
    if (!dragging_)
        return;
    const QPainterPath path = previewPath();
    if (path.isEmpty())
        return;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 190), 0));
    p.drawPath(path);
    p.setPen(QPen(QColor(255, 255, 255, 225), 0, Qt::DashLine));
    p.drawPath(path);
    Q_UNUSED(info);
}

QWidget *SelectionToolBase::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *modeBox = InkOptions::combo({QStringLiteral("Replace"), QStringLiteral("Add"),
                                       QStringLiteral("Subtract"), QStringLiteral("Intersect")},
                                      page, 106);
    modeBox->setCurrentIndex(int(mode_));
    connect(modeBox, &QComboBox::currentIndexChanged, page,
            [this](int i) { mode_ = SelectionAction(i); });
    InkOptions::addField(row, QStringLiteral("MODE"), modeBox);

    row->addWidget(InkOptions::separator(page));
    auto *antialias = InkOptions::toggle(QStringLiteral("Antialias"), antialias_, page);
    row->addWidget(antialias);
    connect(antialias, &QToolButton::toggled, page, [this](bool on) { antialias_ = on; });

    row->addWidget(InkOptions::separator(page));
    auto *hint = new QLabel(QStringLiteral("Shift add · Alt subtract · Ctrl intersect"), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);

    addExtraOptions(row, page);
    row->addStretch(1);
    return page;
}

void SelectionToolBase::addExtraOptions(QHBoxLayout *row, QWidget *page) {
    Q_UNUSED(row);
    Q_UNUSED(page);
}

Rect SelectionToolBase::documentRect() const {
    if (!ctx_)
        return {};
    const Image &img = ctx_->doc()->image();
    return {0, 0, img.width(), img.height()};
}

// --- rectangle ---
QPainterPath RectSelectTool::previewPath() const {
    QPainterPath path;
    path.addRect(boundsOf(start_, current_));
    return path;
}

std::vector<Vec2> RectSelectTool::polygon() const {
    const QRectF r = boundsOf(start_, current_);
    return {{r.left(), r.top()}, {r.right(), r.top()}, {r.right(), r.bottom()}, {r.left(), r.bottom()}};
}

// --- ellipse ---
QPainterPath EllipseSelectTool::previewPath() const {
    QPainterPath path;
    path.addEllipse(boundsOf(start_, current_));
    return path;
}

std::vector<Vec2> EllipseSelectTool::polygon() const {
    const QRectF r = boundsOf(start_, current_);
    std::vector<Vec2> pts;
    if (r.width() < 0.5 || r.height() < 0.5)
        return pts;
    const double cx = r.center().x(), cy = r.center().y();
    const double rx = r.width() * 0.5, ry = r.height() * 0.5;
    const int steps = 160;
    pts.reserve(size_t(steps));
    for (int i = 0; i < steps; ++i) {
        const double a = 2.0 * M_PI * double(i) / double(steps);
        pts.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
    }
    return pts;
}

// --- lasso ---
QPainterPath LassoSelectTool::previewPath() const {
    QPainterPath path;
    if (path_.size() < 2)
        return path;
    path.moveTo(path_[0].x, path_[0].y);
    for (size_t i = 1; i < path_.size(); ++i)
        path.lineTo(path_[i].x, path_[i].y);
    path.closeSubpath();
    return path;
}

std::vector<Vec2> LassoSelectTool::polygon() const { return path_; }

// Whole-stroke mode: the freehand path is collected exactly like the plain
// lasso (no magnetic edge-clinging); on release, any ink component the loop
// touches is expanded to the complete stroke on the current layer.
void LassoSelectTool::endPrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    if (!wholeStroke_ || !ctx_) {
        SelectionToolBase::endPrimary(e);
        return;
    }
    dragging_ = false;
    current_ = e.docPos;
    Selection &sel = ctx_->selection();
    const std::vector<Vec2> poly = polygon();
    if (poly.size() < 3) {
        if (effectiveAction(e.mods) == SelectionAction::Replace && !sel.isEmpty()) {
            sel.clear();
            ctx_->notifySelectionChanged();
            ctx_->setStatus(QStringLiteral("Deselected"));
        }
        path_.clear();
        ctx_->requestRepaint();
        return;
    }
    // Scan the WHOLE cell, not just its active paint layer: ink drawn on any
    // visible layer of the current cell is grabbable. nullptr = nothing
    // exposed at the playhead; an empty extent = a cell with no visible ink.
    // Both degrade to the plain polygon, but say so instead of failing silently.
    const PaintDevice *device = ctx_->doc() ? ctx_->doc()->compositeOfCurrentCell() : nullptr;
    const bool hasInk = device && !device->extent().isEmpty();
    // Tag the mask with the working track (the adjustment's target when the
    // adjustment is selected), so keyframed ink keeps the ants attached.
    if (PaintLayer *pl = ctx_->doc()->currentLayer())
        sel.setTargetId(pl->id());
    else if (AdjustmentLayer *al = ctx_->doc()->currentAdjustment())
        sel.setTargetId(al->targetId());
    // Re-derive the ants' display map BEFORE the polygon is mapped into the
    // mask. applyPolygon() back-maps a document-space path into device space
    // only while that map is current, and the mask's map is dropped by
    // reset() (Select All, Deselect, a fresh document, the first selection of a
    // session). Syncing only afterwards - as this path used to - meant a
    // selection drawn on a keyframed layer after any of those was stored in
    // DISPLAY space: the mask stopped matching the ink it circled, which is the
    // "ants on their own track" report. The target id above is what the sync
    // reads, so the order here is load-bearing.
    if (ctx_->doc())
        ctx_->doc()->syncSelectionTransform();
    if (hasInk)
        sel.applyStrokePolygon(poly, effectiveAction(e.mods), antialias_, *device, gapRadius_);
    else
        sel.applyPolygon(poly, effectiveAction(e.mods), antialias_);
    path_.clear();
    ctx_->notifySelectionChanged();
    ctx_->requestRepaint();
    const Rect b = sel.bounds();
    if (!hasInk)
        ctx_->setStatus(QStringLiteral("Nothing to select on this frame"));
    else
        ctx_->setStatus(sel.isEmpty()
                            ? QStringLiteral("Selection cleared")
                            : QStringLiteral("Selection %1 × %2 px").arg(b.w).arg(b.h));
}

void LassoSelectTool::addExtraOptions(QHBoxLayout *row, QWidget *page) {
    row->addWidget(InkOptions::separator(page));
    auto *whole = InkOptions::toggle(QStringLiteral("Whole stroke"), wholeStroke_, page);
    whole->setToolTip(QStringLiteral("Select complete strokes touched by the lasso "
                                     "(one pen-down to pen-up = one stroke)"));
    connect(whole, &QToolButton::toggled, page, [this](bool on) {
        wholeStroke_ = on;
        if (ctx_)
            ctx_->setStatus(statusHint());
    });
    row->addWidget(whole);

    auto *radius = InkOptions::slider(0, 28, gapRadius_, page, 70);
    auto *radiusValue = InkOptions::value(QStringLiteral("%1 px").arg(gapRadius_), page, 40);
    radius->setToolTip(QStringLiteral("How close the lasso must pass to count as touching a stroke"));
    InkOptions::addField(row, QStringLiteral("GAP"), radius);
    row->addWidget(radiusValue);
    connect(radius, &QSlider::valueChanged, page, [this, radiusValue](int v) {
        gapRadius_ = v;
        radiusValue->setText(QStringLiteral("%1 px").arg(v));
    });
}

// --- shell actions (Select All / Deselect / Invert) ---
void selectAll(ToolContext &ctx) {
    ctx.selection().selectAll();
    ctx.notifySelectionChanged();
    ctx.setStatus(QStringLiteral("Selected all"));
}

void deselect(ToolContext &ctx) {
    ctx.selection().clear();
    ctx.notifySelectionChanged();
    ctx.setStatus(QStringLiteral("Deselected"));
}

void invertSelection(ToolContext &ctx) {
    ctx.selection().invert();
    ctx.notifySelectionChanged();
    ctx.setStatus(QStringLiteral("Selection inverted"));
}

} // namespace ink
