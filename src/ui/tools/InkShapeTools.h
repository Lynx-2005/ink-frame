#pragma once
// Shape tools: line, rectangle, ellipse. One shared drag-preview/commit
// lifecycle; the commit path goes through the engine's shape rasterizer so a
// stroke follows the brush tip with real spacing instead of stamped discs.
#include "core/InkTileStore.h"
#include "ui/tools/InkTool.h"
#include <QPainterPath>

namespace ink {

class PaintDevice;
class ToolContext;

class ShapeToolBase : public Tool {
public:
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }
    void paintOverlay(QPainter &p, const OverlayInfo &info) const override;
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override {
        return QStringLiteral("Drag to draw · Esc cancels · the shape commits on release");
    }

protected:
    virtual bool supportsFill() const { return true; }
    virtual QPainterPath previewPath(const Vec2 &a, const Vec2 &b) const = 0;
    virtual void rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                        TileMemento &memento, Rect &dirty) const = 0;

    bool dragging_{false};
    Vec2 start_{}, current_{};
};

class LineTool : public ShapeToolBase {
public:
    QString id() const override { return QStringLiteral("line"); }

protected:
    bool supportsFill() const override { return false; }
    QPainterPath previewPath(const Vec2 &a, const Vec2 &b) const override;
    void rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                TileMemento &memento, Rect &dirty) const override;
};

class RectangleTool : public ShapeToolBase {
public:
    QString id() const override { return QStringLiteral("rectangle"); }

protected:
    QPainterPath previewPath(const Vec2 &a, const Vec2 &b) const override;
    void rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                TileMemento &memento, Rect &dirty) const override;
};

class EllipseTool : public ShapeToolBase {
public:
    QString id() const override { return QStringLiteral("ellipse"); }

protected:
    QPainterPath previewPath(const Vec2 &a, const Vec2 &b) const override;
    void rasterize(ToolContext &ctx, PaintDevice &dev, const Vec2 &a, const Vec2 &b,
                TileMemento &memento, Rect &dirty) const override;
};

} // namespace ink
