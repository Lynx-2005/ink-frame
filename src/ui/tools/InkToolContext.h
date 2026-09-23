#pragma once
// ToolContext: the shared state every tool works against — document handle,
// foreground/background colors, brush settings, shape settings and the
// dirty-rect plumbing back into the canvas. One place owns it, so the
// toolbox, the options bar and the tools can never drift apart.
#include "resources/InkResources.h"
#include "core/InkTileStore.h"
#include <QColor>
#include <QObject>
#include <QString>

namespace ink {

class Document;
class PaintDevice;
struct ToolHost;

class ToolContext : public QObject {
    Q_OBJECT
public:
    explicit ToolContext(Document *doc, QObject *parent = nullptr);

    void setHost(ToolHost *host) { host_ = host; }
    Document *doc() const { return doc_; }
    int frame() const;

    // Current layer/frame paint target; nullptr when the layer is locked,
    // missing or not paintable.
    PaintDevice *editableDevice() const;
    bool canEdit() const { return editableDevice() != nullptr; }

    // --- colors: foreground / background (X swaps, D resets) ---
    const QColor &primary() const { return primary_; }
    const QColor &secondary() const { return secondary_; }
    void setPrimaryColor(const QColor &c);
    void setSecondaryColor(const QColor &c);
    void swapColors();
    void resetColors();

    // --- brush settings (paint tools) ---
    const BrushPreset &preset() const { return preset_; }
    const ResourceDb &resources() const { return db_; }
    // Swap tip/dynamics to a named preset; keeps the current size.
    bool setPresetId(const QString &id);
    double size() const { return double(preset_.tip.diameter); }
    void setSize(double px);
    void setOpacity(int percent);
    void setHardness(int percent);
    void setSpacing(int percent);
    void setPressureSize(bool on);
    void setPressureOpacity(bool on);
    void setSmoothing(int percent);

    // --- shape tools: own stroke width, never the brush size ---
    double shapeWidth() const { return shapeWidth_; }
    void setShapeWidth(double px);
    bool shapeFill() const { return shapeFill_; }
    void setShapeFill(bool on);

    // --- painting plumbing ---
    void markDirty(const Rect &r);
    Rect takeDirty();
    void requestRepaint();
    void requestRebuild();
    // Publish an undoable edit: memento (before) + after-tiles (redo),
    // then refresh the canvas projection and let the shell sync.
    void pushStrokeCommand(const QString &name, PaintDevice *device, TileMemento memento);
    void setStatus(const QString &text) { emit statusChanged(text); }

signals:
    void colorsChanged();
    void settingsChanged(); // brush tip/dynamics or shape options changed
    void strokeCommitted();
    void statusChanged(const QString &text);

private:
    Document *doc_;
    ToolHost *host_{nullptr};
    ResourceDb db_;
    BrushPreset preset_;
    QColor primary_{20, 22, 26};
    QColor secondary_{255, 255, 255};
    double shapeWidth_{8.0};
    bool shapeFill_{false};
    Rect pendingDirty_;
};

} // namespace ink
