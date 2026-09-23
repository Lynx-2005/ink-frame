#pragma once
// Tool options bar: the active tool's identity plus its own option widget,
// swapped on every tool change, with a right-aligned status readout.
#include <QWidget>

class QLabel;
class QStackedWidget;

namespace ink {

class ToolManager;

class ToolOptionsBar : public QWidget {
public:
    explicit ToolOptionsBar(ToolManager *tools, QWidget *parent = nullptr);

    void setStatus(const QString &text);

private:
    void showTool(const QString &id);

    ToolManager *tools_;
    QLabel *icon_{nullptr};
    QLabel *name_{nullptr};
    QLabel *status_{nullptr};
    QStackedWidget *stack_{nullptr};
};

} // namespace ink
