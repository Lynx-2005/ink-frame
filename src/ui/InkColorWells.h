#pragma once
// Foreground/background color swatches with swap (X) and reset (D).
// Overlapping squares, flat 1px borders — the tool chrome idiom, not candy.
#include <QWidget>

class QToolButton;

namespace ink {

class ToolContext;
class ToolManager;

class ColorWells : public QWidget {
public:
    ColorWells(ToolContext *ctx, ToolManager *tools, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {60, 40}; }
};

} // namespace ink
