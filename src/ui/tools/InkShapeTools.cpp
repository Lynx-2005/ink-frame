#include "ui/tools/InkShapeTools.h"
#include "brush/InkShapeRaster.h"
#include "core/InkPaintDevice.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {

AutoBrushTip shapeTip(const ToolContext &ctx) {
    // Solid, evenly spaced tip: shapes read as geometry, not as a smear.
    return AutoBrushTip(
        BrushTipShape{float(ctx.shapeWidth()), 0.85f, 1.0f, 0.08f});
}

PixelRGBA8 shapeColor(const ToolContext &ctx) {
    const QColor c = ctx.primary();
    return {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
}

QRectF boundsOf(const Vec2 &a, const Vec2 &b) {
    return QRectF(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                  QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));
}

} // namespace

void ShapeToolBase::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    dragging_ = false;
}

void ShapeToolBase::deactivate() { dragging_ = false; }

void ShapeToolBase::beginPrimary(const ToolEvent &e) {
    if (!ctx_)
        return;
    dragging_ = true;
    start_ = current_ = e.docPos;
    ctx_->requestRepaint();
}

void ShapeToolBase::continuePrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    current_ = e.docPos;
    ctx_->requestRepaint();
}

void ShapeToolBase::endPrimary(const ToolEvent &e) {
    if (!dragging_)
        return;
    dragging_ = false;
    current_ = e.docPos;
    PaintDevice *device = ctx_->editableDevice();
    if (!device) {
        ctx_->setStatus(ctx_->blockedHint());
        return;
    }
    TileMemento memento;
    Rect dirty;
    rasterize(*ctx_, *device, start_, current_, memento, dirty);
    if (!memento.empty()) {
        ctx_->markDirty(dirty);
        ctx_->pushStrokeCommand(QStringLiteral("Shape"), device, std::move(memento));
    } else {
        ctx_->requestRepaint();
    }
}

void ShapeToolBase::cancelPrimary() {
    if (!dragging_)
        return;
    dragging_ = false;
    ctx_->requestRepaint();
}

void ShapeToolBase::paintOverlay(QPainter &p, const OverlayInfo &info) const {
    if (!dragging_)
        return;
    const QPainterPath path = previewPath(start_, current_);
    if (ctx_->shapeFill() && supportsFill())
        p.fillPath(path, QColor(232, 232, 234, 40));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(232, 232, 234, 210), 0));
    p.drawPath(path);
    Q_UNUSED(info);
}

QWidget *ShapeToolBase::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *width = InkOptions::slider(1, 64, int(ctx_->shapeWidth() + 0.5), page, 90);
    auto *widthVal = InkOptions::value(QStringLiteral("%1 px").arg(width->value()), page);
    InkOptions::addField(row, QStringLiteral("WIDTH"), width);
    row->addWidget(widthVal);

    QToolButton *fill = nullptr;
    if (supportsFill()) {
        row->addWidget(InkOptions::separator(page));
        fill = InkOptions::toggle(QStringLiteral("Fill"), ctx_->shapeFill(), page);
        row->addWidget(fill);
    }
    row->addStretch(1);

    connect(width, &QSlider::valueChanged, page, [this, widthVal](int v) {
        widthVal->setText(QStringLiteral("%1 px").arg(v));
        ctx_->setShapeWidth(v);
    });
    if (fill)
        connect(fill, &QToolButton::toggled, page, [this](bool on) { ctx_->setShapeFill(on); });

    connect(ctx_, &ToolContext::settingsChanged, page, [this, width, widthVal, fill] {
        const QSignalBlocker b1(width);
        width->setValue(int(ctx_->shapeWidth() + 0.5));
        widthVal->setText(QStringLiteral("%1 px").arg(width->value()));
        if (fill) {
            const QSignalBlocker b2(fill);
            fill->setChecked(ctx_->shapeFill());
        }
    });

    return page;
}

// --- line ---
QPainterPath LineTool::previewPath(const Vec2 &a, const Vec2 &b) const {
    QPainterPath path;
    path.moveTo(a.x, a.y);
    path.lineTo(b.x, b.y);
    return path;
}

void LineTool::rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                      TileMemento &memento, Rect &dirty) const {
    dirty = strokePolyline(dev, {{a.x, a.y}, {b.x, b.y}}, shapeTip(ctx), shapeColor(ctx), 1.0f,
                           CompositeOp::Normal, &memento, ctx.selectionClip());
}

// --- rectangle ---
QPainterPath RectangleTool::previewPath(const Vec2 &a, const Vec2 &b) const {
    QPainterPath path;
    path.addRect(boundsOf(a, b));
    return path;
}

void RectangleTool::rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                           TileMemento &memento, Rect &dirty) const {
    const QRectF r = boundsOf(a, b);
    if (ctx.shapeFill()) {
        const int x0 = int(std::floor(r.left()));
        const int y0 = int(std::floor(r.top()));
        const int x1 = int(std::ceil(r.right()));
        const int y1 = int(std::ceil(r.bottom()));
        dirty = dev.fillRect({x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)},
                             shapeColor(ctx), CompositeOp::Normal, &memento, ctx.selectionClip());
        return;
    }
    dirty = strokeRect(dev, a.x, a.y, b.x, b.y, shapeTip(ctx), shapeColor(ctx), 1.0f,
                       CompositeOp::Normal, &memento, ctx.selectionClip());
}

// --- ellipse ---
QPainterPath EllipseTool::previewPath(const Vec2 &a, const Vec2 &b) const {
    QPainterPath path;
    path.addEllipse(boundsOf(a, b));
    return path;
}

void EllipseTool::rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                         TileMemento &memento, Rect &dirty) const {
    const QRectF r = boundsOf(a, b);
    const double cx = (r.left() + r.right()) * 0.5;
    const double cy = (r.top() + r.bottom()) * 0.5;
    const double rx = r.width() * 0.5;
    const double ry = r.height() * 0.5;
    if (ctx.shapeFill()) {
        dirty = fillEllipse(dev, cx, cy, rx, ry, shapeColor(ctx), CompositeOp::Normal, &memento,
                            ctx.selectionClip());
        return;
    }
    dirty = strokeEllipse(dev, cx, cy, rx, ry, shapeTip(ctx), shapeColor(ctx), 1.0f,
                          CompositeOp::Normal, &memento, ctx.selectionClip());
}

} // namespace ink
