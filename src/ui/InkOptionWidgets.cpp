#include "ui/InkOptionWidgets.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace ink {
namespace InkOptions {

namespace {

// Drop-down chevron drawn in code (InkIcons): once ::drop-down is styled Qt
// skips its own arrow glyph, and this app ships no image assets.
class InkCombo : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent *e) override {
        QComboBox::paintEvent(e);
        QStyleOptionComboBox opt;
        opt.initFrom(this);
        QRect cell =
            style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxArrow, this);
        if (cell.isEmpty()) cell = QRect(width() - 24, 0, 24, height());
        QPainter p(this);
        const QColor ink = !isEnabled() ? InkTheme::faint()
                                        : (underMouse() ? InkTheme::text() : InkTheme::dim());
        InkIcons::drawChevron(p, cell, /*up=*/false, ink, 9.0);
    }
};

void applyToggleIcon(QToolButton *button) {
    button->setIcon(button->isChecked()
                        ? InkIcons::get(IconId::Check, InkTheme::toolGlyphOn(), 14)
                        : InkIcons::get(IconId::CheckBox, InkTheme::toolGlyph(), 14));
}

} // namespace

QLabel *caption(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("OptCaption"));
    return l;
}

QLabel *value(const QString &text, QWidget *parent, int minWidth) {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("OptValue"));
    l->setMinimumWidth(minWidth);
    // Readouts hug their slider's right edge, so rows never look ragged.
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return l;
}

QSlider *slider(int min, int max, int val, QWidget *parent, int width) {
    auto *s = new QSlider(Qt::Horizontal, parent);
    s->setRange(min, max);
    s->setValue(val);
    s->setFixedWidth(width);
    return s;
}

QComboBox *combo(const QStringList &items, QWidget *parent, int width) {
    auto *c = new InkCombo(parent);
    c->addItems(items);
    c->setFixedWidth(width);
    return c;
}

QToolButton *toggle(const QString &text, bool checked, QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("OptToggle"));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setIconSize(QSize(14, 14));
    b->setCheckable(true);
    b->setChecked(checked);
    b->setFocusPolicy(Qt::NoFocus);
    applyToggleIcon(b);
    QObject::connect(b, &QToolButton::toggled, b, [b] { applyToggleIcon(b); });
    return b;
}

QToolButton *button(const QString &text, QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("OptToggle"));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QFrame *separator(QWidget *parent) {
    auto *f = new QFrame(parent);
    f->setObjectName(QStringLiteral("OptSep"));
    f->setFrameShape(QFrame::NoFrame);
    f->setFixedWidth(1);
    f->setMinimumHeight(18);
    return f;
}

void addField(QHBoxLayout *layout, const QString &caption, QWidget *control) {
    layout->addWidget(InkOptions::caption(caption, control->parentWidget()));
    layout->addWidget(control);
}

void addSection(QVBoxLayout *page, const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("OptSection"));
    page->addWidget(l);
    auto *rule = new QFrame(parent);
    rule->setObjectName(QStringLiteral("OptSepH"));
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    page->addWidget(rule);
}

void formRow(QGridLayout *grid, int row, const QString &caption, QWidget *control,
             QWidget *trailing) {
    auto *c = InkOptions::caption(caption, grid->parentWidget());
    c->setFixedWidth(kFormCaptionWidth);
    c->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(c, row, 0);
    grid->addWidget(control, row, 1);
    if (trailing)
        grid->addWidget(trailing, row, 2);
}

} // namespace InkOptions
} // namespace ink
