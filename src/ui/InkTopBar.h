#pragma once
// TopBar per concept art: logo | project dropdown || undo redo | zoom | layers.
// Emits zoomChanged(factor index) and undo/redo signals; MainWindow wires
// zoom to the shared ViewConverter (single source of truth).
// The right-most button (three dots) toggles the right-side Layer panel.
#include <QWidget>

class QComboBox;
class QToolButton;

namespace ink {

class TopBar : public QWidget {
    Q_OBJECT
public:
    explicit TopBar(QWidget *parent = nullptr);
    void setZoom(double z);               // 1.0 = 100%
    void setRotation(double degrees);     // canvas rotation readout
    void setLayersChecked(bool on);       // keep the toggle in step with F7
signals:
    void undoRequested();
    void redoRequested();
    void zoomPicked(double z);
    void rotatePicked(double degrees);
    void layersToggled(bool on);
private:
    QComboBox *projBox_{nullptr};
    QComboBox *zoomBox_{nullptr};
    QComboBox *rotBox_{nullptr};
    QToolButton *layersBtn_{nullptr};
};

} // namespace ink
