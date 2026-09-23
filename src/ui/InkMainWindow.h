#pragma once
// Main window (Kosei concept art): TopBar + [ToolRail | CanvasCard]
// + TimelineCard. Canvas + timeline dock, file menus (export),
// undo/redo, onion toggle, brush presets via tool rail.
#include <QMainWindow>

namespace ink {
class Document;
class CanvasWidget;
class TimelineDock;
class TopBar;
class ToolRail;
class ToolOptions;

enum class ToolId : int;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onExportPngSeq();
    void onExportMp4();
    void onUndo();
    void onRedo();
    void onToolChanged(int tool);
    void onColorsChanged(QColor a, QColor b);
    void onZoomPicked(double z);
    void onViewChanged();

private:
    Document *doc_{nullptr};
    CanvasWidget *canvas_{nullptr};
    TimelineDock *timeline_{nullptr};
    TopBar *topBar_{nullptr};
    ToolRail *rail_{nullptr};
    ToolOptions *toolOpts_{nullptr};
};

} // namespace ink
