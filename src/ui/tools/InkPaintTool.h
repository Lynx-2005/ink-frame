#pragma once
// Freehand paint tools. PaintToolBase owns the stroke lifecycle shared by
// Brush and Eraser (the Krita concept: one freehand event stream, and the
// eraser is the same tool with an erase composite op at the engine level).
#include "brush/InkPaintOp.h"
#include "core/InkTileStore.h"
#include "resources/InkResources.h" // BrushPreset
#include "ui/tools/InkTool.h"
#include <QStringList>
#include <memory>

namespace ink {

class ToolContext;

class PaintToolBase : public Tool {
public:
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    bool isEditing() const override { return stroking_; }
    QCursor cursor() const override;
    void paintOverlay(QPainter &p, const OverlayInfo &info) const override;
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override { return QStringLiteral("Drag on the canvas to paint"); }

protected:
    virtual std::unique_ptr<PaintOp> makeOp(const BrushPreset &preset) const = 0;
    virtual QString strokeName() const = 0;
    // Presets offered in the options bar; fewer than two hides the chooser.
    virtual QStringList presetIds() const { return {}; }

private:
    void sendEvent(const ToolEvent &e);
    void rebuildOp();
    bool opMatches(const BrushPreset &p) const;

    std::unique_ptr<PaintOp> op_;
    BrushPreset built_{};
    bool hasBuilt_{false};
    TileMemento memento_;
    Rect opDirty_;
    PaintDevice *device_{nullptr};
    bool stroking_{false};
    BrushPreset strokePreset_{};
    PixelRGBA8 strokeColor_{0, 0, 0, 255};
};

class BrushTool : public PaintToolBase {
public:
    QString id() const override { return QStringLiteral("brush"); }

protected:
    std::unique_ptr<PaintOp> makeOp(const BrushPreset &preset) const override {
        return std::make_unique<PixelBrushOp>(AutoBrushTip(preset.tip), preset.dynamics);
    }
    QString strokeName() const override { return QStringLiteral("Brush stroke"); }
    QStringList presetIds() const override {
        return {QStringLiteral("ink_pen"), QStringLiteral("soft_round"), QStringLiteral("marker")};
    }
};

class EraserTool : public PaintToolBase {
public:
    QString id() const override { return QStringLiteral("eraser"); }

protected:
    std::unique_ptr<PaintOp> makeOp(const BrushPreset &preset) const override {
        return std::make_unique<EraserOp>(AutoBrushTip(preset.tip), preset.dynamics);
    }
    QString strokeName() const override { return QStringLiteral("Eraser stroke"); }
    QString statusHint() const override { return QStringLiteral("Drag on the canvas to erase"); }
};

} // namespace ink
