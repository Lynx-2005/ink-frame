#pragma once
// ParamField: a numeric field that scrubs under the mouse and takes typed
// numbers. Built on QLineEdit so text editing, selection and IME come for
// free; the drag is layered on top and only claims the press if the pointer
// actually moves.
//
// It is the value editor the EASING GRAPH uses for its per-group value strip.
// The old floating TransformPanel that used to live beside it is gone: two
// transform surfaces could disagree, and the toolbar transform plus this graph
// are now the only places a keyed value is edited.
#include "core/InkAdjustmentLayer.h"
#include "core/InkParams.h"
#include "ui/InkParamFormat.h" // transformGroups(): the five dropdown entries
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QWidget>

class QComboBox;

namespace ink {

class Document;

// A numeric field that scrubs under the mouse and takes typed numbers. Built on
// QLineEdit so text editing, selection and IME come for free; the drag is layered
// on top and only claims the press if the pointer actually moves.
class ParamField : public QLineEdit {
    Q_OBJECT
public:
    explicit ParamField(ParamId id, QWidget *parent = nullptr);

    ParamId param() const { return id_; }
    void showValue(double v); // set the display WITHOUT emitting a change
    double value() const { return value_; }
    void setReadOnly2(bool ro) { setReadOnly(ro); }

signals:
    void scrubbed(double value);  // live, during a drag
    void committed(double value); // typed / Enter / wheel / drag end

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void applyStep(int notches, bool coarse);

    ParamId id_;
    double value_{0.0};
    double dragStartValue_{0.0};
    int dragStartX_{0};
    bool pressSeen_{false};
    bool dragging_{false};
};

} // namespace ink
