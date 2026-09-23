#pragma once
// Left toolbox: two-column icon grid, hairline-separated tool sections, flat
// square buttons whose checked state is a solid steel-blue block, plus the
// foreground/background wells. Collapsible to a slim strip (state remembered).
#include <QWidget>

class QToolButton;

namespace ink {

class ToolContext;
class ToolManager;

class ToolBox : public QWidget {
public:
    ToolBox(ToolManager *tools, ToolContext *ctx, QWidget *parent = nullptr);

private:
    void setCollapsed(bool collapsed);

    QToolButton *collapse_{nullptr};
    QWidget *body_{nullptr};
    bool collapsed_{false};
};

} // namespace ink
