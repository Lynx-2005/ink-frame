#pragma once
// Selection tools: rectangle, ellipse and freehand lasso. They all share the
// drag-preview + commit-to-mask lifecycle; the mask itself lives on the
// document, so painting and filling are clipped by it. The lasso can run in
// whole-stroke mode, where touching any part of a drawn stroke (one pen-down
// to pen-up counts as one stroke) selects the complete stroke.
#include "core/InkSelection.h"
#include "ui/tools/InkTool.h"
#include <QPainterPath>
#include <vector>

class QHBoxLayout;

namespace ink {

class ToolContext;

class SelectionToolBase : public Tool {
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
        return QStringLiteral("Drag to select · Shift adds · Alt subtracts · Ctrl intersects");
    }

protected:
    // Outline of the selection being dragged, in document space.
    virtual QPainterPath previewPath() const = 0;
    // Final polygon (document space, implicitly closed); < 3 points = click.
    virtual std::vector<Vec2> polygon() const = 0;
    // Tool-specific controls appended to the shared options row.
    virtual void addExtraOptions(QHBoxLayout *row, QWidget *page);

    SelectionAction effectiveAction(Qt::KeyboardModifiers mods) const;
    Rect documentRect() const;
    ToolContext *context() const { return ctx_; }

    bool dragging_{false};
    Vec2 start_{}, current_{};
    std::vector<Vec2> path_; // lasso samples
    SelectionAction mode_{SelectionAction::Replace};
    bool antialias_{true};
};

class RectSelectTool : public SelectionToolBase {
public:
    QString id() const override { return QStringLiteral("rect_select"); }

protected:
    QPainterPath previewPath() const override;
    std::vector<Vec2> polygon() const override;
};

class EllipseSelectTool : public SelectionToolBase {
public:
    QString id() const override { return QStringLiteral("ellipse_select"); }

protected:
    QPainterPath previewPath() const override;
    std::vector<Vec2> polygon() const override;
};

class LassoSelectTool : public SelectionToolBase {
public:
    QString id() const override { return QStringLiteral("lasso"); }
    void endPrimary(const ToolEvent &e) override;
    QString statusHint() const override {
        return wholeStroke_
                   ? QStringLiteral("Whole stroke: circle any part of a stroke to select all of it")
                   : QStringLiteral("Drag to select · Shift adds · Alt subtracts · Ctrl intersects");
    }

protected:
    QPainterPath previewPath() const override;
    std::vector<Vec2> polygon() const override;
    void addExtraOptions(QHBoxLayout *row, QWidget *page) override;

private:
    // Whole-stroke mode (off = plain freehand area). Touching any pixel of a
    // stroke selects the complete stroke on the current layer.
    bool wholeStroke_{false};
    // Forgiveness radius: the lasso counts as touching ink within this many
    // pixels of its edge, so a rough circle still grabs the stroke.
    int gapRadius_{10};
};

// Shared helpers for the shell actions (Select All / Deselect / Invert).
void selectAll(ToolContext &ctx);
void deselect(ToolContext &ctx);
void invertSelection(ToolContext &ctx);

} // namespace ink
