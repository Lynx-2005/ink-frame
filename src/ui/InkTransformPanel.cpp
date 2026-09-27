// ParamField, implemented. See InkTransformPanel.h for why it is the only
// value editor left: the floating TransformPanel that shared this file is gone,
// and the easing graph's value strip is what remains.
#include "ui/InkTransformPanel.h"
#include "ui/InkDocument.h"
#include "ui/InkParamFormat.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ink {

namespace {
QFont uiFont(int px, bool bold = false) {
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

} // namespace

// --- ParamField --------------------------------------------------------------

ParamField::ParamField(ParamId id, QWidget *parent) : QLineEdit(parent), id_(id) {
    setObjectName(QStringLiteral("ParamField"));
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setFont(uiFont(12));
    setFixedHeight(26);
    setMaximumWidth(96);
    setCursorPosition(0);
    // A field that grabs focus on press would swallow the drag, so the press is
    // claimed manually once the pointer actually moves.
    setFocusPolicy(Qt::ClickFocus);
    // Enter or a focus-out commits what was typed. Parsing lives here, not in
    // the panel: the field owns the text, so the field owns reading it back.
    connect(this, &QLineEdit::editingFinished, this, [this] {
        if (dragging_) return; // a drag already emitted with the clamped value
        bool ok = false;
        const double v = parseParamValue(id_, text(), &ok);
        if (!ok) {
            setText(formatParamValue(id_, value_)); // bad text, restore the number
            return;
        }
        const ParamDesc &d = paramDesc(id_);
        const double c = std::clamp(v, d.lo, d.hi);
        if (std::abs(c - value_) < 1e-12) {
            setText(formatParamValue(id_, value_));
            return;
        }
        value_ = c;
        setText(formatParamValue(id_, c));
        emit committed(c);
    });
}

void ParamField::showValue(double v) {
    value_ = v;
    const QString text = formatParamValue(id_, v);
    const bool hadSel = hasSelectedText();
    const int cp = cursorPosition();
    setText(text);
    // Painting a new value into a field the user is typing in must not move
    // their caret or select their text.
    if (hadSel) selectAll();
    else setCursorPosition(cp);
}

void ParamField::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) {
        pressSeen_ = true;
        dragging_ = false;
        dragStartX_ = e->pos().x();
        dragStartValue_ = value_;
    }
    QLineEdit::mousePressEvent(e);
}

void ParamField::mouseMoveEvent(QMouseEvent *e) {
    if (pressSeen_ && (e->buttons() & Qt::LeftButton) &&
        std::abs(e->pos().x() - dragStartX_) > 3) {
        // A real drag: take the focus away from the text caret and scrub.
        if (!dragging_) {
            dragging_ = true;
            clearFocus();
        }
        const ParamDesc &d = paramDesc(id_);
        const double span = d.hi - d.lo;
        // A drag across the field's own width moves ~5% of the range by
        // default; Shift makes it a full sweep. Slow enough to land exactly.
        const double perPx = span * ((e->modifiers() & Qt::ShiftModifier) ? 0.01 : 0.0015);
        double v = dragStartValue_ + double(e->pos().x() - dragStartX_) * perPx;
        v = std::clamp(v, d.lo, d.hi);
        if (std::abs(v - value_) < 1e-12) return;
        value_ = v;
        setText(formatParamValue(id_, v));
        emit scrubbed(v);
        return;
    }
    QLineEdit::mouseMoveEvent(e);
}

void ParamField::mouseReleaseEvent(QMouseEvent *e) {
    if (dragging_) {
        dragging_ = false;
        emit committed(value_);
    } else if (pressSeen_ && e->button() == Qt::LeftButton) {
        // A click with no drag: select everything so the next keystroke
        // replaces the number — the Alight Motion behaviour.
        selectAll();
        setFocus(Qt::MouseFocusReason);
    }
    pressSeen_ = false;
    QLineEdit::mouseReleaseEvent(e);
}

void ParamField::applyStep(int notches, bool coarse) {
    const ParamDesc &d = paramDesc(id_);
    const double span = d.hi - d.lo;
    double step = span / 400.0;
    // Round the raw step to 1/2/5 x 10^n so a wheel lands on clean numbers.
    const double mag = std::pow(10.0, std::floor(std::log10(std::max(step, 1e-9))));
    const double norm = step / mag;
    const double mult = norm > 5.0 ? 10.0 : (norm > 2.0 ? 5.0 : (norm > 1.0 ? 2.0 : 1.0));
    step = mult * mag;
    if (coarse) step *= 10.0;
    double v = std::clamp(value_ + step * notches, d.lo, d.hi);
    if (std::abs(v - value_) < 1e-12) return;
    value_ = v;
    setText(formatParamValue(id_, v));
    emit committed(v);
}

void ParamField::wheelEvent(QWheelEvent *e) {
    const int notches = e->angleDelta().y() / 120;
    if (notches == 0) return;
    applyStep(notches, e->modifiers() & Qt::ShiftModifier);
    e->accept();
}

void ParamField::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        applyStep(e->key() == Qt::Key_Up ? 1 : -1, e->modifiers() & Qt::ShiftModifier);
        e->accept();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

} // namespace ink
