#pragma once
// Left toolbox: two-column icon grid, hairline-separated tool sections,
// flat square buttons whose checked state is a solid steel-blue block.
// Buttons are bound to the ToolManager QActions, so name, icon, shortcut,
// tooltip and checked state all come from one place.
#include <QWidget>

namespace ink {

class ToolContext;
class ToolManager;

class ToolBox : public QWidget {
public:
    ToolBox(ToolManager *tools, ToolContext *ctx, QWidget *parent = nullptr);
};

} // namespace ink
