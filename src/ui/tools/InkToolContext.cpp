#include "ui/tools/InkToolContext.h"
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

void ToolContext::setPrimaryColor(const QColor &c) {
    if (!c.isValid() || c == primary_)
        return;
    primary_ = c;
    emit colorsChanged();
}

Selection &ToolContext::selection() { return doc_->selection(); }

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
    if (host_)
        host_->rebuild();
    emit strokeCommitted();
}

} // namespace ink
