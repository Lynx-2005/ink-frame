#include "ui/InkCanvasWidget.h"
#include "animation/InkAnimation.h"
#include "ui/InkDocument.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <cstring>
#include <vector>

namespace ink {

CanvasWidget::CanvasWidget(Document *doc, QWidget *parent)
    : QOpenGLWidget(parent), doc_(doc) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    view_.setDocSize(double(doc_->image().width()), double(doc_->image().height()));
}

void CanvasWidget::setBrushPreset(const BrushPreset &p) {
    preset_ = p; // value copy — canvas owns it, no lifetime coupling
    hasPreset_ = true;
    if (p.engine == "eraser" || eraser_)
        op_ = std::make_unique<EraserOp>(AutoBrushTip(p.tip), p.dynamics);
    else
        op_ = std::make_unique<PixelBrushOp>(AutoBrushTip(p.tip), p.dynamics);
}

void CanvasWidget::setBrushColor(const QColor &c) {
    preset_.color = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
}

void CanvasWidget::setToolMode(ToolId t) {
    toolMode_ = t;
    shaping_ = false;
    update();
}

void CanvasWidget::setBrushSize(double px) {
    brushSize_ = std::clamp(px, 1.0, 256.0);
    preset_.tip.diameter = float(brushSize_);
    if (hasPreset_) {
        if (preset_.engine == "eraser" || eraser_)
            op_ = std::make_unique<EraserOp>(AutoBrushTip(preset_.tip), preset_.dynamics);
        else
            op_ = std::make_unique<PixelBrushOp>(AutoBrushTip(preset_.tip), preset_.dynamics);
    }
    update();
}

// Rasterize a shape preview onto the editable device as one undoable op.
void CanvasWidget::commitShape() {
    if (!shaping_) return;
    shaping_ = false;
    if (!doc_->currentLayer() || doc_->currentLayer()->locked()) { update(); return; }
    PaintDevice *dev = doc_->editableDevice(doc_->currentLayer(), doc_->currentFrame());
    if (!dev) { update(); return; }
    TileMemento m;
    const PixelRGBA8 col = preset_.color;
    const float r = float(brushSize_ * 0.5);
    Rect acc;
    auto disc = [&](float cx, float cy) {
        int x0 = int(std::floor(cx - r)), x1 = int(std::ceil(cx + r));
        int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                if (dx * dx + dy * dy <= r * r)
                    acc = uniteRect(acc, dev->fillRect({x, y, 1, 1}, col, CompositeOp::Normal, &m));
            }
    };
    auto line = [&](QPointF a, QPointF b) {
        double len = std::hypot(b.x() - a.x(), b.y() - a.y());
        int steps = std::max(1, int(len / std::max(1.0, r * 0.5)));
        for (int i = 0; i <= steps; ++i) {
            double t = double(i) / steps;
            disc(float(a.x() + (b.x() - a.x()) * t), float(a.y() + (b.y() - a.y()) * t));
        }
    };
    if (shapeMode_ == ShapeMode::Line) {
        line(shapeStart_, shapeCur_);
    } else {
        double x0 = std::min(shapeStart_.x(), shapeCur_.x());
        double y0 = std::min(shapeStart_.y(), shapeCur_.y());
        double x1 = std::max(shapeStart_.x(), shapeCur_.x());
        double y1 = std::max(shapeStart_.y(), shapeCur_.y());
        if (shapeMode_ == ShapeMode::Rect) {
            line({x0, y0}, {x1, y0});
            line({x1, y0}, {x1, y1});
            line({x1, y1}, {x0, y1});
            line({x0, y1}, {x0, y0});
        } else {
            double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
            double rx = (x1 - x0) * 0.5, ry = (y1 - y0) * 0.5;
            double per = 2 * M_PI * std::hypot(rx, ry) / 1.4142;
            int steps = std::max(24, int(per / std::max(1.0, r * 0.5)));
            for (int i = 0; i <= steps; ++i) {
                double a = 2 * M_PI * i / steps;
                disc(float(cx + rx * std::cos(a)), float(cy + ry * std::sin(a)));
            }
        }
    }
    if (!m.empty()) {
        StrokeCommand cmd;
        cmd.name = "Shape";
        cmd.targets.push_back({dev, std::move(m)});
        for (auto &e : cmd.targets[0].memento.entries)
            cmd.after.push_back({dev, e.pos, dev->store().tile(e.pos.tx, e.pos.ty)});
        doc_->undo().push(std::move(cmd));
        strokeDirty_ = uniteRect(strokeDirty_, acc);
        update(); // incremental path patches just the shape bounds
        doc_->touchLayer(doc_->currentLayer(), doc_->currentFrame());
        markDirty();
        emit strokeFinished();
    } else {
        update();
    }
}

void CanvasWidget::pickAt(const QPointF &cp) {
    PaintDevice proj;
    doc_->image().project(doc_->currentFrame(), proj);
    int x = int(std::floor(cp.x())), y = int(std::floor(cp.y()));
    if (x < 0 || y < 0 || x >= doc_->image().width() || y >= doc_->image().height()) return;
    PixelRGBA8 px = proj.pixel(x, y);
    if (px.a == 0) return;
    emit colorPicked(QColor(px.r, px.g, px.b));
}

void CanvasWidget::fitToView() {
    syncViewSizes();
    view_.fit();
    fitted_ = true;
    emit viewChanged();
    update();
}

void CanvasWidget::markDirty() {
    composedDirty_ = true;
    ghostDevs_.clear(); // layer content changed somewhere: ghosts may be stale
    update();
}

void CanvasWidget::setZoom(double z) {
    syncViewSizes();
    if (z < 0.02 || z > 64.0) return;
    double cur = view_.zoom();
    if (cur > 0) view_.zoomAtCenter(z / cur);
    emit viewChanged();
    update();
}

void CanvasWidget::initializeGL() {
    initializeOpenGLFunctions();
}

void CanvasWidget::resizeGL(int w, int h) {
    view_.setWidgetSize(double(w), double(h));
    if (!fitted_) {
        view_.fit();
        fitted_ = true;
    }
}

void CanvasWidget::syncViewSizes() {
    view_.setDocSize(double(doc_->image().width()), double(doc_->image().height()));
    view_.setWidgetSize(double(width()), double(height()));
    view_.setDevicePixelRatio(double(devicePixelRatioF()));
}

QPointF CanvasWidget::toCanvas(const QPointF &w) const {
    Vec2 d = view_.mapToDoc({w.x(), w.y()});
    return {d.x, d.y};
}

QTransform CanvasWidget::qtransform() const {
    const Affine2D &t = view_.docToWidget();
    return {t.a, t.b, t.c, t.d, t.tx, t.ty};
}

std::shared_ptr<PaintDevice> CanvasWidget::frameImage(int frame, bool live) {
    if (!live) {
        if (auto cached = doc_->frameCache().get(frame))
            return cached;
    }
    // Synchronous fallback (also the live path during a stroke) + warm the
    // cache for scrub/playback.
    auto out = std::make_shared<PaintDevice>();
    doc_->image().project(frame, *out);
    doc_->frameCache().requestRange(frame);
    return out;
}

std::shared_ptr<PaintDevice> CanvasWidget::ghostImage(int frame) {
    // Ghost frames don't change while stroking the current frame, so cache
    // their projections across rebuilds within the same playhead position.
    // Cleared on frame change / external dirty (see rebuildComposed).
    auto it = ghostDevs_.find(frame);
    if (it != ghostDevs_.end()) return it->second;
    auto dev = frameImage(frame, false);
    if ((int)ghostDevs_.size() > 32) ghostDevs_.clear();
    ghostDevs_[frame] = dev;
    return dev;
}

void CanvasWidget::applyGhosts(PaintDevice &composed, const Rect *clip) {
    const int frame = doc_->currentFrame();
    const OnionSkinConfig &onion = doc_->onion();
    if (!onion.enabled) return;
    for (int i = onion.prevCount; i >= 1; --i) {
        int f = frame - i;
        if (f < 0) continue;
        float op = onion.maxOpacity * (1.0f - float(i - 1) / float(onion.prevCount + 1));
        if (auto g = ghostImage(f))
            compositeOnionSkin(composed, *g, onion.prevTint, op, clip, onion.tintEnabled);
    }
    for (int i = 1; i <= onion.nextCount; ++i) {
        int f = frame + i;
        if (f >= doc_->timeline().frameCount()) continue;
        float op = onion.maxOpacity * (1.0f - float(i - 1) / float(onion.nextCount + 1));
        if (auto g = ghostImage(f))
            compositeOnionSkin(composed, *g, onion.nextTint, op, clip, onion.tintEnabled);
    }
}

void CanvasWidget::uploadRect(const Rect &r) {
    if (r.isEmpty() || composedImage_.isNull()) return;
    Rect canvas{0, 0, composedImage_.width(), composedImage_.height()};
    Rect c = intersectRect(r, canvas);
    if (c.isEmpty()) return;
    // Patch only the dirty rows into the persistent image. QImage is
    // contiguous RGBA8888, so each row patch is one memcpy run.
    const int bpl = composedImage_.bytesPerLine();
    std::vector<uint8_t> buf(size_t(c.w) * size_t(c.h) * 4);
    composedDev_.readRect(c, buf.data());
    const uint8_t *src = buf.data();
    for (int y = 0; y < c.h; ++y, src += size_t(c.w) * 4)
        std::memcpy(composedImage_.bits() + size_t(c.y + y) * bpl + size_t(c.x) * 4, src,
                    size_t(c.w) * 4);
}

void CanvasWidget::rebuildComposed() {
    const int frame = doc_->currentFrame();
    if (frame != composedFrame_) ghostDevs_.clear(); // playhead moved
    // Live project while stroking so each dab is visible immediately;
    // cached (pre-stroke) projection otherwise.
    auto proj = frameImage(frame, stroking_);
    composedDev_.clear();
    // Ghosts FIRST, active frame over them: onion skin is a light table, never
    // a tint on top of the drawing being worked on.
    applyGhosts(composedDev_, nullptr);
    if (proj) composedDev_.blit(*proj, 0, 0);
    // (Re)allocate the persistent full-canvas image and upload everything.
    const int W = doc_->image().width(), H = doc_->image().height();
    if (composedImage_.width() != W || composedImage_.height() != H)
        composedImage_ = QImage(W, H, QImage::Format_RGBA8888);
    composedImage_.fill(Qt::transparent);
    Rect e = composedDev_.extent();
    if (!e.isEmpty()) uploadRect(e);
    strokeDirty_ = Rect{};
    composedFrame_ = frame;
    composedDirty_ = false;
}

void CanvasWidget::incrementalUpdate() {
    // Re-project ONLY the stroke's dirty rect through the layer stack, then
    // re-apply ghosts clipped to it, then patch the image. Cost scales with
    // the stroke's footprint — not with layer count or canvas size (tile
    // sparsity skips untouched layers/tiles).
    Rect r = intersectRect(strokeDirty_, Rect{0, 0, doc_->image().width(), doc_->image().height()});
    strokeDirty_ = Rect{};
    if (r.isEmpty()) return;
    // projectRect tile-aligns internally and returns the actual region. It
    // lands in the scratch (not composedDev_) so the refreshed tiles can be
    // rebuilt in the same order as a full rebuild: ghosts under, frame over.
    Rect refreshed = doc_->image().projectRect(doc_->currentFrame(), r, scratchDev_);
    if (refreshed.isEmpty()) return;
    composedDev_.store().clearRect(refreshed);
    applyGhosts(composedDev_, &refreshed);
    composedDev_.blitRect(scratchDev_, refreshed, refreshed.x, refreshed.y);
    uploadRect(refreshed);
}

void CanvasWidget::paintGL() {
    syncViewSizes();
    if (!fitted_) {
        view_.fit();
        fitted_ = true;
    }
    const int frame = doc_->currentFrame();
    if (composedDirty_ || frame != composedFrame_ || composedImage_.isNull()) {
        rebuildComposed();
    } else if (!strokeDirty_.isEmpty()) {
        incrementalUpdate(); // stroke loop: patch dirty rect only
    }

    QPainter p(this);
    // 1. backdrop gutter in widget space (Kosei dark surround + padding)
    p.fillRect(0, 0, width(), height(), QColor(26, 27, 31));
    const int kPad = 16;
    const double kRadius = 10.0;
    const int W = doc_->image().width(), H = doc_->image().height();
    // Card rect: canvas image fitted inside padding; keep aspect via view.
    // The view transform already centers the doc; derive its widget rect.
    Vec2 tl = view_.mapToWidget({0, 0});
    Vec2 br = view_.mapToWidget({double(W), double(H)});
    QRectF cardR(std::min(tl.x, br.x) - 8, std::min(tl.y, br.y) - 8,
                 std::abs(br.x - tl.x) + 16, std::abs(br.y - tl.y) + 16);
    // Clamp card inside gutter padding.
    cardR = cardR.intersected(QRectF(kPad, kPad, width() - 2 * kPad, height() - 2 * kPad));
    QPainterPath card;
    card.addRoundedRect(cardR, kRadius, kRadius);
    p.fillPath(card, QColor(38, 39, 44));
    // 2. everything else clipped to the rounded card through THE transform
    p.save();
    p.setClipPath(card);
    p.setRenderHint(QPainter::SmoothPixmapTransform, view_.zoom() < 1.0);
    p.setTransform(qtransform());

    // checkerboard over the canvas bounds
    const int cs = 24;
    QColor a(200, 200, 200), b(150, 150, 150);
    for (int y = 0; y < H; y += cs)
        for (int x = 0; x < W; x += cs)
            p.fillRect(QRect(x, y, std::min(cs, W - x), std::min(cs, H - y)),
                       ((x / cs + y / cs) % 2) ? a : b);
    // canvas edge
    p.setPen(QPen(QColor(0, 0, 0, 160), 0));
    p.drawRect(QRect(0, 0, W, H));
    // composed projection (persistent full-canvas image, patched
    // incrementally mid-stroke)
    if (!composedImage_.isNull())
        p.drawImage(QPoint(0, 0), composedImage_);
    // shape preview + brush cursor ring (document space, cosmetic pens)
    {
        QPen prevPen(QColor(224, 138, 148, 230), 0);
        p.setPen(prevPen);
        p.setBrush(Qt::NoBrush);
        if (shaping_) {
            if (shapeMode_ == ShapeMode::Line) {
                p.drawLine(shapeStart_, shapeCur_);
            } else {
                QRectF sr(QPointF(std::min(shapeStart_.x(), shapeCur_.x()),
                                  std::min(shapeStart_.y(), shapeCur_.y())),
                          QPointF(std::max(shapeStart_.x(), shapeCur_.x()),
                                  std::max(shapeStart_.y(), shapeCur_.y())));
                if (shapeMode_ == ShapeMode::Rect) p.drawRect(sr);
                else p.drawEllipse(sr);
            }
        } else if (hover_ && !stroking_ &&
                   (toolMode_ == ToolId::Brush || toolMode_ == ToolId::Eraser ||
                    toolMode_ == ToolId::Shape)) {
            double rad = brushSize_ * 0.5;
            p.setPen(QPen(QColor(255, 255, 255, 210), 0));
            p.drawEllipse(hoverPos_, rad, rad);
            p.setPen(QPen(QColor(224, 138, 148, 180), 0));
            p.drawEllipse(hoverPos_, rad + 1.5 / std::max(0.05, view_.zoom()),
                          rad + 1.5 / std::max(0.05, view_.zoom()));
        }
    }
    p.restore(); // un-clip
    // card edge highlight
    p.setPen(QPen(QColor(52, 53, 60), 1));
    p.drawPath(card);
    // empty-state hint (no content yet): guide toward folder/track flow
    if (composedImage_.isNull() || composedDev_.extent().isEmpty()) {
        p.setPen(QColor(150, 152, 160));
        p.drawText(cardR, Qt::AlignCenter, QStringLiteral("+ Draw on Layer 1 — add keys in the timeline"));
    }
    p.end();
}

void CanvasWidget::beginStrokeAt(const QPointF &cp, float pressure, double timeSec) {
    if (!doc_->currentLayer() || doc_->currentLayer()->locked()) return;
    if (!hasPreset_) {
        ResourceDb db = ResourceDb::defaults(); // local lives through the copy
        setBrushPreset(*db.find("ink_pen"));
    }
    PaintDevice *dev = doc_->editableDevice(doc_->currentLayer(), doc_->currentFrame());
    if (!dev) return;
    strokeMemento_ = TileMemento{}; // fresh per stroke — undo never bleeds
    strokeDevice_ = dev;
    // Dabs union their rects into strokeDirty_; paintGL consumes it via the
    // incremental path (no full rebuild per dab, no matter how many layers).
    PaintOpContext ctx{dev, preset_.color, CompositeOp::Normal, &strokeMemento_, &strokeDirty_};
    if (!op_) setBrushPreset(preset_);
    op_->beginStroke(ctx);
    stroking_ = true;
    StrokeEvent ev;
    ev.state.x = float(cp.x());
    ev.state.y = float(cp.y());
    ev.state.pressure = pressure;
    ev.timeSec = timeSec;
    op_->addEvent(ev);
    update();
}

void CanvasWidget::moveStrokeTo(const QPointF &cp, float pressure, double timeSec) {
    if (!stroking_) return;
    StrokeEvent ev;
    ev.state.x = float(cp.x());
    ev.state.y = float(cp.y());
    ev.state.pressure = pressure;
    ev.timeSec = timeSec;
    op_->addEvent(ev);
    update(); // strokeDirty_ accumulated; paintGL patches incrementally
}

void CanvasWidget::endStroke() {
    if (!stroking_) return;
    op_->endStroke();
    stroking_ = false;
    // Publish a real undo command: memento (before) + after-tiles (redo).
    if (strokeDevice_ && !strokeMemento_.empty()) {
        StrokeCommand cmd;
        cmd.name = eraser_ ? "Eraser stroke" : "Brush stroke";
        cmd.targets.push_back({strokeDevice_, std::move(strokeMemento_)});
        for (auto &e : cmd.targets[0].memento.entries)
            cmd.after.push_back({strokeDevice_, e.pos, strokeDevice_->store().tile(e.pos.tx, e.pos.ty)});
        doc_->undo().push(std::move(cmd));
    }
    strokeDevice_ = nullptr;
    doc_->touchLayer(doc_->currentLayer(), doc_->currentFrame());
    markDirty(); // converge: full rebuild off the fresh cache, ghosts refreshed
    emit strokeFinished();
}

void CanvasWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        if (toolMode_ == ToolId::Picker) { pickAt(toCanvas(e->position())); return; }
        if (toolMode_ == ToolId::Shape) {
            shaping_ = true;
            shapeStart_ = shapeCur_ = toCanvas(e->position());
            update();
            return;
        }
        beginStrokeAt(toCanvas(e->position()), 0.6f, double(e->timestamp()) / 1000.0);
    } else if (e->button() == Qt::MiddleButton) {
        panStart_ = e->pos();
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *e) {
    hover_ = true;
    hoverPos_ = toCanvas(e->position());
    if (shaping_) {
        shapeCur_ = toCanvas(e->position());
        update();
    } else if (stroking_) {
        moveStrokeTo(toCanvas(e->position()), 0.6f, double(e->timestamp()) / 1000.0);
    } else if (e->buttons() & Qt::MiddleButton) {
        QPoint d = e->pos() - panStart_;
        panStart_ = e->pos();
        view_.panBy({double(d.x()), double(d.y())});
        emit viewChanged();
        update();
    } else {
        update(); // cursor ring follows
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        if (shaping_) { commitShape(); return; }
        endStroke();
    }
}

void CanvasWidget::leaveEvent(QEvent *e) {
    hover_ = false;
    QOpenGLWidget::leaveEvent(e);
    update();
}

void CanvasWidget::wheelEvent(QWheelEvent *e) {
    const float f = e->angleDelta().y() > 0 ? 1.15f : 1.0f / 1.15f;
    QPointF w = e->position();
    view_.zoomAt({w.x(), w.y()}, double(f)); // cursor-anchored (still-point)
    emit viewChanged();
    update();
}

void CanvasWidget::tabletEvent(QTabletEvent *e) {
    switch (e->type()) {
    case QEvent::TabletPress:
        if (toolMode_ == ToolId::Picker) { pickAt(toCanvas(e->position())); e->accept(); break; }
        if (toolMode_ == ToolId::Shape) {
            shaping_ = true;
            shapeStart_ = shapeCur_ = toCanvas(e->position());
            update();
            e->accept();
            break;
        }
        beginStrokeAt(toCanvas(e->position()), float(e->pressure()),
                      double(e->timestamp()) / 1000.0);
        e->accept();
        break;
    case QEvent::TabletMove:
        if (shaping_) {
            shapeCur_ = toCanvas(e->position());
            update();
        } else if (stroking_)
            moveStrokeTo(toCanvas(e->position()), float(e->pressure()),
                         double(e->timestamp()) / 1000.0);
        e->accept();
        break;
    case QEvent::TabletRelease:
        if (shaping_) commitShape();
        else endStroke();
        e->accept();
        break;
    default:
        break;
    }
}

void CanvasWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) {
        view_.zoomAtCenter(1.15);
        update();
    } else if (e->key() == Qt::Key_Minus) {
        view_.zoomAtCenter(1.0 / 1.15);
        update();
    } else if (e->key() == Qt::Key_0 && (e->modifiers() & Qt::ControlModifier)) {
        fitToView();
    }
}

} // namespace ink
