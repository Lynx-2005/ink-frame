#pragma once
// Main window (Kosei concept art): TopBar + [ToolBox | CanvasCard | LayerPanel]
// + TimelineCard. Owns the document, the tool context and the tool manager;
// all cross-widget wiring lives here.
// There is deliberately NO menu bar: the top bar's undo/redo/zoom/rotate
// controls and the Layer panel cover what File/Edit/View used to hold.
#include <QMainWindow>

namespace ink {
class CanvasWidget;
class Document;
class LayerPanel;
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
    void onExportPngSeq(); // kept for later re-homing; no UI entry point
    void onExportMp4();
    void onUndo();
    void onRedo();
    void onZoomPicked(double z);
    void onRotatePicked(double deg);
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
    LayerPanel *layerPanel_{nullptr};
    QAction *layersAction_{nullptr};
    // The layer panel's real state. The top-bar button and the F7 action MIRROR
    // this; neither is allowed to drive it, or the two drift apart and a press
    // stops matching what the panel actually does.
    bool layerPanelVisible_{true};
};

} // namespace ink
