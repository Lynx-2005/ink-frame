#include "ui/tools/InkToolFactory.h"
#include "ui/tools/InkColorPickerTool.h"
#include "ui/tools/InkFillTool.h"
#include "ui/tools/InkMoveTool.h"
#include "ui/tools/InkPaintTool.h"
#include "ui/tools/InkSelectionTools.h"
#include "ui/tools/InkShapeTools.h"
#include <algorithm>

namespace ink {

ToolRegistry &ToolRegistry::instance() {
    static ToolRegistry reg;
    return reg;
}

void ToolRegistry::add(ToolFactory f) {
    for (auto &existing : factories_) {
        if (existing.id == f.id) {
            existing = std::move(f);
            return;
        }
    }
    factories_.push_back(std::move(f));
    std::stable_sort(factories_.begin(), factories_.end(),
                     [](const ToolFactory &a, const ToolFactory &b) {
                         if (a.section != b.section)
                             return a.section < b.section;
                         if (a.priority != b.priority)
                             return a.priority < b.priority;
                         return a.name < b.name;
                     });
}

const ToolFactory *ToolRegistry::find(const QString &id) const {
    for (auto &f : factories_)
        if (f.id == id)
            return &f;
    return nullptr;
}

void registerBuiltinTools() {
    ToolRegistry &reg = ToolRegistry::instance();

    ToolFactory brush;
    brush.id = QStringLiteral("brush");
    brush.name = QStringLiteral("Brush");
    brush.section = ToolSections::paint();
    brush.icon = IconId::Brush;
    brush.shortcut = QKeySequence(Qt::Key_B);
    brush.priority = 0;
    brush.create = [] { return new BrushTool(); };
    reg.add(brush);

    ToolFactory eraser;
    eraser.id = QStringLiteral("eraser");
    eraser.name = QStringLiteral("Eraser");
    eraser.section = ToolSections::paint();
    eraser.icon = IconId::Eraser;
    eraser.shortcut = QKeySequence(Qt::Key_E);
    eraser.priority = 1;
    eraser.create = [] { return new EraserTool(); };
    reg.add(eraser);

    ToolFactory fill;
    fill.id = QStringLiteral("fill");
    fill.name = QStringLiteral("Fill");
    fill.section = ToolSections::fill();
    fill.icon = IconId::Fill;
    fill.shortcut = QKeySequence(Qt::Key_F);
    fill.priority = 0;
    fill.create = [] { return new FillTool(); };
    reg.add(fill);

    ToolFactory picker;
    picker.id = QStringLiteral("picker");
    picker.name = QStringLiteral("Color Picker");
    picker.section = ToolSections::fill();
    picker.icon = IconId::Picker;
    picker.shortcut = QKeySequence(Qt::Key_P);
    picker.priority = 1;
    picker.create = [] { return new ColorPickerTool(); };
    reg.add(picker);

    ToolFactory line;
    line.id = QStringLiteral("line");
    line.name = QStringLiteral("Line");
    line.section = ToolSections::shape();
    line.icon = IconId::Line;
    line.shortcut = QKeySequence(Qt::Key_L);
    line.priority = 0;
    line.create = [] { return new LineTool(); };
    reg.add(line);

    ToolFactory rectangle;
    rectangle.id = QStringLiteral("rectangle");
    rectangle.name = QStringLiteral("Rectangle");
    rectangle.section = ToolSections::shape();
    rectangle.icon = IconId::Rectangle;
    rectangle.shortcut = QKeySequence(Qt::Key_R);
    rectangle.priority = 1;
    rectangle.create = [] { return new RectangleTool(); };
    reg.add(rectangle);

    ToolFactory ellipse;
    ellipse.id = QStringLiteral("ellipse");
    ellipse.name = QStringLiteral("Ellipse");
    ellipse.section = ToolSections::shape();
    ellipse.icon = IconId::Ellipse;
    ellipse.shortcut = QKeySequence(Qt::Key_O);
    ellipse.priority = 2;
    ellipse.create = [] { return new EllipseTool(); };
    reg.add(ellipse);

    ToolFactory rectSelect;
    rectSelect.id = QStringLiteral("rect_select");
    rectSelect.name = QStringLiteral("Rectangle Select");
    rectSelect.section = ToolSections::select();
    rectSelect.icon = IconId::SelectRect;
    rectSelect.shortcut = QKeySequence(Qt::Key_M);
    rectSelect.priority = 0;
    rectSelect.create = [] { return new RectSelectTool(); };
    reg.add(rectSelect);

    ToolFactory ellipseSelect;
    ellipseSelect.id = QStringLiteral("ellipse_select");
    ellipseSelect.name = QStringLiteral("Ellipse Select");
    ellipseSelect.section = ToolSections::select();
    ellipseSelect.icon = IconId::SelectEllipse;
    ellipseSelect.shortcut = QKeySequence(Qt::Key_J);
    ellipseSelect.priority = 1;
    ellipseSelect.create = [] { return new EllipseSelectTool(); };
    reg.add(ellipseSelect);

    ToolFactory lasso;
    lasso.id = QStringLiteral("lasso");
    lasso.name = QStringLiteral("Freehand Select");
    lasso.section = ToolSections::select();
    lasso.icon = IconId::Lasso;
    lasso.shortcut = QKeySequence(Qt::SHIFT | Qt::Key_L);
    lasso.priority = 2;
    lasso.create = [] { return new LassoSelectTool(); };
    reg.add(lasso);

    // The selection tools define the region; this is what drags the pixels.
    ToolFactory move;
    move.id = QStringLiteral("move");
    move.name = QStringLiteral("Move");
    move.section = ToolSections::transform();
    move.icon = IconId::Move;
    move.shortcut = QKeySequence(Qt::Key_V);
    move.priority = 0;
    move.create = [] { return new MoveTool(); };
    reg.add(move);
}

} // namespace ink
