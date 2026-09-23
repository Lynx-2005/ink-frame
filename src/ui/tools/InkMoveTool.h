#pragma once
// Move tool: the selection tools define the region, this is what drags it -
// the Krita/Photoshop split (lasso selects, move tool moves the pixels).
// Alt leaves a copy behind, Shift constrains to one axis, arrows nudge.
#include "core/InkRegion.h"
#include "ui/tools/InkTool.h"

namespace ink {

class ToolContext;

class MoveTool : public Tool {
public:
    QString id() const override { return QStringLiteral("move"); }
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    bool nudge(int dx, int dy, Qt::KeyboardModifiers mods) override;
    void flipSelection(bool horizontal) override { flip(horizontal); }
    bool isEditing() const override { return dragging_; }
    QCursor cursor() const override { return QCursor(Qt::SizeAllCursor); }
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override {
        return QStringLiteral("Drag to move the selection · Alt copies · Shift constrains · arrows nudge");
    }

private:
    void resetState();
    bool grab(bool useSelection);
    bool grabAt(const Vec2 &at);
    Rect documentRect() const;
    const Selection *mask() const;
    void applyOffset(int dx, int dy);
    void flip(bool horizontal);
    QString commandName() const;

    ToolContext *ctx_{nullptr};
    PaintDevice *device_{nullptr};
    FloatRegion lifted_;
    TileMemento memento_;
    Rect area_{};
    bool dragging_{false};
    bool copying_{false};
    bool liftedOnce_{false};
    bool selectionScope_{false};
    bool hadSelection_{false};
    Vec2 start_{};
    int offsetX_{0}, offsetY_{0};
};

} // namespace ink
