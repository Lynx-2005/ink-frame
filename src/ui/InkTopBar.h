#pragma once
// TopBar per concept art: logo | project dropdown || undo redo | zoom | menu.
// Emits zoomChanged(factor index) and undo/redo signals; MainWindow wires
// zoom to the shared ViewConverter (single source of truth).
#include <QWidget>

class QComboBox;
class QToolButton;

namespace ink {

class TopBar : public QWidget {
    Q_OBJECT
public:
    explicit TopBar(QWidget *parent = nullptr);
    void setZoom(double z); // 1.0 = 100%
signals:
    void undoRequested();
    void redoRequested();
    void zoomPicked(double z);
    void menuRequested();
private:
    QComboBox *projBox_{nullptr};
    QComboBox *zoomBox_{nullptr};
};

} // namespace ink
