#include "ui/tools/InkToolFactory.h"
#include "ui/tools/InkColorPickerTool.h"
#include "ui/tools/InkPaintTool.h"
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

    ToolFactory picker;
    picker.id = QStringLiteral("picker");
    picker.name = QStringLiteral("Color Picker");
    picker.section = ToolSections::sample();
    picker.icon = IconId::Picker;
    picker.shortcut = QKeySequence(Qt::Key_P);
    picker.priority = 0;
    picker.create = [] { return new ColorPickerTool(); };
    reg.add(picker);
}

} // namespace ink
