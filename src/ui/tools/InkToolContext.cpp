#include "ui/tools/InkToolContext.h"
#include "core/InkParams.h"
#include "ui/InkDocument.h"
#include "ui/tools/InkTool.h"
#include <algorithm>

namespace ink {

ToolContext::ToolContext(Document *doc, QObject *parent) : QObject(parent), doc_(doc) {
    db_ = ResourceDb::defaults();
    if (const BrushPreset *pen = db_.find("ink_pen"))
        preset_ = *pen;
}

int ToolContext::frame() const { return doc_->currentFrame(); }

PaintDevice *ToolContext::editableDevice() const {
    PaintLayer *layer = doc_->currentLayer();
    if (!layer || layer->locked())
        return nullptr;
    return doc_->editableDevice(layer, doc_->currentFrame());
}

bool ToolContext::autoKeyBlocked() const {
    PaintLayer *layer = doc_->currentLayer();
    if (!layer || layer->locked() || doc_->autoKeyEnabled())
        return false;
    return layer->exposureAt(doc_->currentFrame()) < 0;
}

QString ToolContext::blockedHint() const {
    if (autoKeyBlocked())
        return QStringLiteral("Autokey is off — enable auto-key or select a key cell to paint");
    return QStringLiteral("Layer is locked or not paintable");
}

void ToolContext::setPrimaryColor(const QColor &c) {
    if (!c.isValid() || c == primary_)
        return;
    primary_ = c;
    emit colorsChanged();
}

Selection &ToolContext::selection() { return doc_->selection(); }

void ToolContext::notifySelectionChanged() {
    // A new mask is authored in DEVICE space; on a keyframed layer the ink it
    // clips is DISPLACED at composite time, so the ants have to be handed that
    // map before anyone draws them. Frame changes, undo and key edits already
    // re-synced it - changing the selection itself did not, which is why a
    // freshly drawn lasso outline sat beside a rotated stroke instead of around
    // it.
    doc_->syncSelectionTransform();
    emit selectionChanged();
}

const Selection *ToolContext::selectionClip() const {
    const Selection &s = doc_->selection();
    return s.isEmpty() ? nullptr : &s;
}

void ToolContext::setSecondaryColor(const QColor &c) {
    if (!c.isValid() || c == secondary_)
        return;
    secondary_ = c;
    emit colorsChanged();
}

void ToolContext::swapColors() {
    std::swap(primary_, secondary_);
    emit colorsChanged();
}

void ToolContext::resetColors() {
    primary_ = QColor(20, 22, 26);
    secondary_ = QColor(255, 255, 255);
    emit colorsChanged();
}

bool ToolContext::setPresetId(const QString &id) {
    const BrushPreset *p = db_.find(id.toStdString());
    if (!p)
        return false;
    const float keepSize = preset_.tip.diameter;
    preset_ = *p;
    preset_.tip.diameter = keepSize; // presets never change the working size
    emit settingsChanged();
    return true;
}

void ToolContext::setSize(double px) {
    preset_.tip.diameter = float(std::clamp(px, 1.0, 256.0));
    emit settingsChanged();
}

void ToolContext::setOpacity(int percent) {
    preset_.tip.opacity = float(std::clamp(percent, 1, 100)) / 100.0f;
    emit settingsChanged();
}

void ToolContext::setHardness(int percent) {
    preset_.tip.hardness = float(std::clamp(percent, 0, 100)) / 100.0f;
    emit settingsChanged();
}

void ToolContext::setSpacing(int percent) {
    preset_.tip.spacing = float(std::clamp(percent, 1, 100)) / 100.0f;
    emit settingsChanged();
}

void ToolContext::setPressureSize(bool on) {
    preset_.dynamics.sizePressure.enabled = on;
    emit settingsChanged();
}

void ToolContext::setPressureOpacity(bool on) {
    preset_.dynamics.opacityPressure.enabled = on;
    emit settingsChanged();
}

void ToolContext::setSmoothing(int percent) {
    preset_.dynamics.smoothing.amount = float(std::clamp(percent, 0, 100)) / 100.0f;
    emit settingsChanged();
}

void ToolContext::setShapeWidth(double px) {
    shapeWidth_ = std::clamp(px, 1.0, 64.0);
    emit settingsChanged();
}

void ToolContext::setShapeFill(bool on) {
    if (shapeFill_ == on)
        return;
    shapeFill_ = on;
    emit settingsChanged();
}

void ToolContext::markDirty(const Rect &r) {
    if (r.isEmpty())
        return;
    pendingDirty_ = pendingDirty_.isEmpty() ? r : uniteRect(pendingDirty_, r);
    if (host_)
        host_->repaint();
}

Rect ToolContext::takeDirty() {
    Rect r = pendingDirty_;
    pendingDirty_ = Rect{};
    return r;
}

void ToolContext::requestRepaint() {
    if (host_)
        host_->repaint();
}

void ToolContext::requestRebuild() {
    pendingDirty_ = Rect{};
    if (host_)
        host_->rebuild();
}

void ToolContext::pushStrokeCommand(const QString &name, PaintDevice *device, TileMemento memento) {
    if (!device || memento.empty())
        return;
    StrokeCommand cmd;
    cmd.name = name.toStdString();
    cmd.targets.push_back({device, std::move(memento)});
    for (auto &e : cmd.targets[0].memento.entries)
        cmd.after.push_back({device, e.pos, device->store().tile(e.pos.tx, e.pos.ty)});
    doc_->undo().push(std::move(cmd));
    if (PaintLayer *layer = doc_->currentLayer())
        doc_->touchLayer(layer, doc_->currentFrame());
    // Release path: repaint, NOT rebuild. The canvas already holds every dab
    // via the incremental path (markDirty during the stroke + the final flush
    // each tool marks before calling here); a full rebuild would synchronously
    // re-project the frame plus onion ghosts and re-upload the whole image —
    // the click-release stall. Callers that need a full rebuild (undo, layer
    // ops) call host_->rebuild() explicitly.
    if (host_)
        host_->repaint();
    emit strokeCommitted();
}

// --- keyframe recording ------------------------------------------------------

AdjustmentLayer *ToolContext::recordTarget() const {
    if (doc_->currentAdjustment()) return doc_->currentAdjustment();
    if (PaintLayer *l = doc_->currentLayer()) return doc_->adjustmentFor(l);
    return nullptr;
}

bool ToolContext::recording() const {
    return doc_->autoKeyEnabled() && recordTarget() != nullptr;
}

double ToolContext::recordedValue(ParamId id, int frame) const {
    AdjustmentLayer *a = recordTarget();
    if (!a) return 0.0;
    return a->params().valueAt(id, double(frame));
}

void ToolContext::beginParamRecord(const QString &label, int frame) {
    AdjustmentLayer *a = recordTarget();
    if (!a) return;
    doc_->beginParamRecord(label.toStdString(), a);
    (void)frame;
}

void ToolContext::recordParam(ParamId id, double value, int frame) {
    doc_->recordParamValue(id, value, frame);
}

void ToolContext::endParamRecord() {
    if (!doc_->recordActive()) return;
    doc_->endParamRecord();
    // The pixels never moved, but the picture did: the composite has to be
    // rebuilt (not just repainted) and the timeline has to show the new keys.
    if (host_)
        host_->rebuild();
    emit animationRecorded();
}

bool ToolContext::recordActive() const { return doc_->recordActive(); }

// --- camera recording --------------------------------------------------------
// Same one-undo-step contract as the paint path, on the camera undo stream.
// Every entry repaints the canvas (the picture moved) and nudges the timeline
// lane, so the diamond lands on press — not after the next hover repaint.

CameraLayer *ToolContext::cameraTarget() const {
    std::vector<CameraLayer*> all;
    doc_->collectCameraLayers(all);
    if (all.empty()) return nullptr;
    for (CameraLayer *c : all)
        if (c->scope() == CameraScope::Global && c->visible() && !c->locked()) return c;
    for (CameraLayer *c : all)
        if (c->visible() && !c->locked()) return c;
    return all.front();
}

void ToolContext::beginCameraRecord(const QString &label, CameraLayer *cam) {
    if (!cam) return;
    doc_->beginCameraRecord(label.toStdString(), cam);
}

void ToolContext::recordCameraValue(CameraLayer *cam, ParamId id, double value, int frame) {
    if (!cam) return;
    doc_->recordCameraValue(cam, id, value, frame);
    if (host_) host_->rebuild();
    emit cameraPreviewChanged();
}

void ToolContext::endCameraRecord() {
    doc_->endCameraRecord(); // no-op when no record is open
    if (host_) host_->rebuild();
    emit animationRecorded();
}

void ToolContext::cancelCameraRecord() {
    doc_->cancelCameraRecord();
    if (host_) host_->rebuild();
    emit cameraPreviewChanged();
}

bool ToolContext::cameraRecordActive() const {
    std::vector<CameraLayer*> all;
    doc_->collectCameraLayers(all);
    for (CameraLayer *c : all)
        if (doc_->cameraRecordActive(c)) return true;
    return false;
}

void ToolContext::notifyCameraPreview() { emit cameraPreviewChanged(); }

} // namespace ink
