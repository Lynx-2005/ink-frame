#pragma once
// Slim tool-options strip docked above the canvas card. Shows per-tool
// controls for the core four: Brush/Eraser size, Shape mode+size,
// Picker hint. Text/Note/Select are honest placeholders in the rail.
#include <QWidget>
#include "ui/InkCanvasWidget.h" // ShapeMode (no cycle: canvas never includes this)

class QComboBox;
class QLabel;
class QSlider;
class QStackedWidget;

namespace ink {

class ToolOptions : public QWidget {
    Q_OBJECT
public:
    explicit ToolOptions(QWidget *parent = nullptr);
    void setTool(ToolId t);
    void setBrushSize(double px);
signals:
    void brushSizeChanged(double px);
    void shapeModeChanged(ShapeMode m);
private:
    QStackedWidget *stack_{nullptr};
    QWidget *brushPage_{nullptr};
    QWidget *shapePage_{nullptr};
    QWidget *pickerPage_{nullptr};
    QSlider *sizeSlider_{nullptr};
    QSlider *shapeSizeSlider_{nullptr};
    QLabel *sizeLabel_{nullptr};
    QLabel *shapeSizeLabel_{nullptr};
    QComboBox *modeBox_{nullptr};
};

} // namespace ink
