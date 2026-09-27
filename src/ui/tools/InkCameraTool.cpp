#include "ui/tools/InkCameraTool.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkToolContext.h"
#include <QHBoxLayout>
#include <QLabel>

namespace ink {

void CameraTool::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    ctx_ = &ctx;
    cam_ = nullptr;
    dragging_ = false;
}

void CameraTool::deactivate() { cancelPrimary(); }

CameraLayer *CameraTool::resolveTarget() const {
    if (!ctx_ || !ctx_->doc()) return nullptr;
    // Tools never create layers on their own — creation stays in the
    // layer/folder right-click menus. The context shares this resolution so
    // record-state queries agree with the tool by construction.
    return ctx_->cameraTarget();
}

QString CameraTool::statusHint() const {
    return QStringLiteral(
        "Drag to pan · Ctrl+drag up/down to zoom · Shift+drag left/right to rotate · "
        "arrows nudge (right-click a layer/folder for Add Camera)");
}

QWidget *CameraTool::createOptionsWidget(QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *lay = new QHBoxLayout(w);
    lay->setContentsMargins(6, 0, 6, 0);
    auto *lab = new QLabel(QStringLiteral("Camera: pan / zoom / rotate writes keys"), w);
    lay->addWidget(lab);
    return w;
}

void CameraTool::beginPrimary(const ToolEvent &e) {
    if (!ctx_) return;
    cam_ = resolveTarget();
    if (!cam_) {
        ctx_->setStatus(QStringLiteral("No camera yet — right-click a layer or folder and choose Add Camera"));
        return;
    }
    if (cam_->locked()) {
        ctx_->setStatus(QStringLiteral("Camera is locked"));
        cam_ = nullptr;
        return;
    }
    frame_ = ctx_->doc()->currentFrame();
    start_ = last_ = e.docPos;
    dragging_ = true;
    ctx_->beginCameraRecord(QStringLiteral("Move camera"), cam_);
    // Key on press, not on first move: the diamond lands instantly and a
    // click without a drag still leaves a key (one undo step on release).
    ctx_->recordCameraValue(cam_, ParamId::PositionX,
                            cam_->params().valueAt(ParamId::PositionX, frame_), frame_);
    ctx_->recordCameraValue(cam_, ParamId::PositionY,
                            cam_->params().valueAt(ParamId::PositionY, frame_), frame_);
}

void CameraTool::continuePrimary(const ToolEvent &e) {
    if (!dragging_ || !cam_ || !ctx_) return;
    const Vec2 d{e.docPos.x - last_.x, e.docPos.y - last_.y};
    last_ = e.docPos;
    const bool zoomMode = bool(e.mods & Qt::ControlModifier);
    const bool rotMode = bool(e.mods & Qt::ShiftModifier);
    if (zoomMode && !rotMode) {
        // Vertical drag dollies: up = in, down = out. Exponential feel so a
        // long drag stays controllable (factor per pixel, clamped).
        const double factor = std::exp(-d.y * 0.005);
        const double f = std::clamp(factor, 0.5, 2.0);
        double sx = cam_->params().valueAt(ParamId::ScaleX, frame_);
        double sy = cam_->params().valueAt(ParamId::ScaleY, frame_);
        ctx_->recordCameraValue(cam_, ParamId::ScaleX, sx * f, frame_);
        ctx_->recordCameraValue(cam_, ParamId::ScaleY, sy * f, frame_);
    } else if (rotMode && !zoomMode) {
        ctx_->recordCameraValue(cam_, ParamId::Rotation,
                                cam_->params().valueAt(ParamId::Rotation, frame_) + d.x * 0.2,
                                frame_);
    } else {
        // Pan follows the pointer 1:1 in document space (a camera move is the
        // inverse of a layer move on screen, but the stored Position IS the
        // screen delta, so no inversion here).
        ctx_->recordCameraValue(cam_, ParamId::PositionX,
                                cam_->params().valueAt(ParamId::PositionX, frame_) + d.x,
                                frame_);
        ctx_->recordCameraValue(cam_, ParamId::PositionY,
                                cam_->params().valueAt(ParamId::PositionY, frame_) + d.y,
                                frame_);
    }
    // recordCameraValue already rebuilt the canvas and nudged the timeline.
}

void CameraTool::endPrimary(const ToolEvent &) {
    if (!dragging_) return;
    dragging_ = false;
    // One undo step for the whole gesture; the shell refreshes the timeline.
    if (ctx_ && cam_) ctx_->endCameraRecord();
    cam_ = nullptr;
}

void CameraTool::cancelPrimary() {
    if (!dragging_) return;
    dragging_ = false;
    if (ctx_ && cam_) ctx_->cancelCameraRecord();
    cam_ = nullptr;
}

bool CameraTool::nudge(int dx, int dy, Qt::KeyboardModifiers mods) {
    if (!ctx_) return false;
    CameraLayer *c = resolveTarget();
    if (!c || c->locked()) return false;
    Document *doc = ctx_->doc();
    const int f = doc->currentFrame();
    const double step = (mods & Qt::ShiftModifier) ? 10.0 : 1.0;
    return doc->cameraPanBy(c, f, dx * step, dy * step);
}

} // namespace ink
