#pragma once
// Tool options bar: the active tool's identity plus its own option widget,
// swapped on every tool change, with a right-aligned status readout.
#include <QLabel>
#include <QWidget>

class QStackedWidget;

namespace ink {

class ToolManager;

// Status readout that ellipsizes instead of running off the bar: long tool
// hints used to clip mid-word at the window edge.
class ElidedLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
    void setFullText(const QString &t);

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    void updateElided();
    QString full_;
};

class ToolOptionsBar : public QWidget {
public:
    explicit ToolOptionsBar(ToolManager *tools, QWidget *parent = nullptr);

    void setStatus(const QString &text);

private:
    void showTool(const QString &id);

    ToolManager *tools_;
    QLabel *icon_{nullptr};
    QLabel *name_{nullptr};
    ElidedLabel *status_{nullptr};
    QStackedWidget *stack_{nullptr};
};

} // namespace ink
