#include "ui/InkCanvasWidget.h"
#include "animation/InkAnimation.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkToolManager.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
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
    antsTimer_ = new QTimer(this);
    antsTimer_->setInterval(110);
    connect(antsTimer_, &QTimer::timeout, this, [this] {
        antsPhase_ = (antsPhase_ + 1) % 8;
        update();
    });
}

void CanvasWidget::setToolManager(ToolManager *tools) {
    tools_ = tools;
    if (tools_) {
        connect(tools_, &ToolManager::activeToolChanged, this, [this](const QString &) {
            updateCursor();
            update();
        });
    }
    if (ctx_) {
        connect(ctx_, &ToolContext::selectionChanged, this, [this] {
            updateAnts();
            update();
        });
    }
    updateCursor();
}

void CanvasWidget::setToolContext(ToolContext *ctx) {
    ctx_ = ctx;
    if (ctx_)
        ctx_->setHost(this);
}

void CanvasWidget::updateAnts() {
    if (!antsTimer_)
        return;
    const bool active = ctx_ && !ctx_->doc()->selection().isEmpty();
    if (active && !antsTimer_->isActive())
        antsTimer_->start();
    else if (!active && antsTimer_->isActive())
        antsTimer_->stop();
}

void CanvasWidget::updateCursor() {
    if (panning_) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (spaceHeld_) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    Tool *tool = tools_ ? tools_->activeTool() : nullptr;
    setCursor(tool ? tool->cursor() : QCursor(Qt::ArrowCursor));
}

ToolEvent CanvasWidget::makeEvent(const QPointF &docPos, float pressure, double timeSec,
                                  Qt::KeyboardModifiers mods, bool tablet) const {
    ToolEvent e;
    e.docPos = {docPos.x(), docPos.y()};
    e.pressure = pressure;
    e.timeSec = timeSec;
    e.mods = mods;
    e.tablet = tablet;
    return e;
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
    // Synchronous fallback (also the live path while a tool is editing) +
    // warm the cache for scrub/playback.
    auto out = std::make_shared<PaintDevice>();
    doc_->image().project(frame, *out);
    doc_->frameCache().requestRange(frame);
    return out;
}

std::shared_ptr<PaintDevice> CanvasWidget::ghostImage(int frame) {
    // Ghost frames don't change while editing the current frame, so cache
    // their projections across rebuilds within the same playhead position.
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

void CanvasWidget::rebuildComposed(bool live) {
    const int frame = doc_->currentFrame();
    if (frame != composedFrame_) ghostDevs_.clear(); // playhead moved
    auto proj = frameImage(frame, live);
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
    composedFrame_ = frame;
    composedDirty_ = false;
    if (ctx_) ctx_->takeDirty(); // the rebuild just consumed any pending patch
}

void CanvasWidget::incrementalUpdate(const Rect &dirty) {
    // Re-project ONLY the dirty rect through the layer stack, then re-apply
    // ghosts clipped to it, then patch the image. Cost scales with the
    // footprint — not with layer count or canvas size (tile sparsity skips
    // untouched layers/tiles).
    Rect r = intersectRect(dirty, Rect{0, 0, doc_->image().width(), doc_->image().height()});
    if (r.isEmpty()) return;
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
    Tool *tool = tools_ ? tools_->activeTool() : nullptr;
    const bool live = tool && tool->isEditing();
    const int frame = doc_->currentFrame();
    if (composedDirty_ || frame != composedFrame_ || composedImage_.isNull()) {
        rebuildComposed(live);
    } else if (ctx_) {
        const Rect dirty = ctx_->takeDirty();
        if (!dirty.isEmpty())
            incrementalUpdate(dirty); // edit loop: patch dirty rect only
    }

    QPainter p(this);
    // 1. backdrop gutter in widget space (dark surround + padding)
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
    // active tool overlay (cursor ring, shape preview) in document space
    if (tool) {
        OverlayInfo info;
        info.zoom = view_.zoom();
        info.hover = hover_;
        info.hoverPos = {hoverPos_.x(), hoverPos_.y()};
        tool->paintOverlay(p, info);
    }
    // marching ants for the active selection (drawn last: it is UI chrome)
    if (!doc_->selection().isEmpty()) {
        const auto &segments = doc_->selection().outline();
        if (!segments.empty()) {
            QPainterPath ants;
            for (const auto &s : segments) {
                ants.moveTo(s[0], s[1]);
                ants.lineTo(s[2], s[3]);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 200), 0));
            p.drawPath(ants);
            QPen light(QColor(255, 255, 255, 235), 0, Qt::CustomDashLine);
            light.setDashPattern({4.0, 4.0});
            light.setDashOffset(double(antsPhase_));
            p.setPen(light);
            p.drawPath(ants);
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

void CanvasWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::MiddleButton || (spaceHeld_ && e->button() == Qt::LeftButton)) {
        panning_ = true;
        panStart_ = e->pos();
        updateCursor();
        return;
    }
    if (e->button() != Qt::LeftButton || !tools_)
        return;
    toolPressed_ = true;
    tools_->press(makeEvent(toCanvas(e->position()), 1.0f, double(e->timestamp()) / 1000.0,
                            e->modifiers(), false));
    update();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *e) {
    hover_ = true;
    hoverPos_ = toCanvas(e->position());
    if (panning_) {
        const QPoint d = e->pos() - panStart_;
        panStart_ = e->pos();
        view_.panBy({double(d.x()), double(d.y())});
        emit viewChanged();
        update();
        return;
    }
    if (toolPressed_ && tools_)
        tools_->move(makeEvent(hoverPos_, 1.0f, double(e->timestamp()) / 1000.0, e->modifiers(), false));
    update(); // ring / preview follow the pointer
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (panning_ && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        panning_ = false;
        updateCursor();
        return;
    }
    if (e->button() == Qt::LeftButton && toolPressed_) {
        toolPressed_ = false;
        if (tools_)
            tools_->release(makeEvent(toCanvas(e->position()), 1.0f, double(e->timestamp()) / 1000.0,
                                      e->modifiers(), false));
        update();
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
    if (!tools_) {
        e->accept();
        return;
    }
    const QPointF docPos = toCanvas(e->position());
    ToolEvent ev = makeEvent(docPos, float(e->pressure()), double(e->timestamp()) / 1000.0,
                             e->modifiers(), true);
    ev.tiltX = float(e->xTilt());
    ev.tiltY = float(e->yTilt());
    switch (e->type()) {
    case QEvent::TabletPress:
        toolPressed_ = true;
        tools_->press(ev);
        break;
    case QEvent::TabletMove:
        hover_ = true;
        hoverPos_ = docPos;
        if (toolPressed_)
            tools_->move(ev);
        break;
    case QEvent::TabletRelease:
        if (toolPressed_)
            tools_->release(ev);
        toolPressed_ = false;
        break;
    default:
        break;
    }
    update();
    e->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceHeld_ = true;
        updateCursor();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        if (tools_)
            tools_->cancel();
        toolPressed_ = false;
        update();
        return;
    }
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

void CanvasWidget::keyReleaseEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        spaceHeld_ = false;
        panning_ = false;
        updateCursor();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(e);
}

} // namespace ink
