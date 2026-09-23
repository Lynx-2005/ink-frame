#pragma once
// Left tool rail per concept art: brush/eraser/shape/picker/text/note/select
// + two color wells. Single-select; emits toolChanged + colors.
#include <QColor>
#include <QList>
#include <QWidget>

class QToolButton;

namespace ink {

enum class ToolId { Brush, Eraser, Shape, Picker, Text, Note, Select };

class ToolRail : public QWidget {
    Q_OBJECT
public:
    explicit ToolRail(QWidget *parent = nullptr);
    ToolId currentTool() const { return tool_; }
    QColor primaryColor() const { return colA_; }
    QColor secondaryColor() const { return colB_; }
    void setPrimaryColor(const QColor &c);
signals:
    void toolChanged(ToolId t);
    void colorsChanged(QColor a, QColor b);
private slots:
    void pickA();
    void pickB();
private:
    void setTool(ToolId t);
    ToolId tool_{ToolId::Brush};
    QColor colA_{224, 138, 148};
    QColor colB_{20, 20, 22};
    QList<QToolButton*> btns_;
    QToolButton *wellA_{nullptr}, *wellB_{nullptr};
    void refreshWells();
};

} // namespace ink
