#pragma once
// Main window (Kosei concept art): TopBar + [ToolBox | CanvasCard]
// + TimelineCard. Owns the document, the tool context and the tool manager;
// all cross-widget wiring lives here.
#include <QMainWindow>

namespace ink {
class CanvasWidget;
class Document;
class TimelineDock;
class ToolBox;
class ToolContext;
class ToolManager;
class ToolOptionsBar;
class TopBar;

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
    void onZoomPicked(double z);
    void onViewChanged();

private:
    Document *doc_{nullptr};
    ToolContext *toolCtx_{nullptr};
    ToolManager *tools_{nullptr};
    CanvasWidget *canvas_{nullptr};
    TimelineDock *timeline_{nullptr};
    TopBar *topBar_{nullptr};
    ToolBox *toolBox_{nullptr};
    ToolOptionsBar *toolOpts_{nullptr};
};

} // namespace ink
