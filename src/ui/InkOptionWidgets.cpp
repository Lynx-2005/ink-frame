#include "ui/InkOptionWidgets.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>

namespace ink {
namespace InkOptions {

namespace {

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
    auto *c = new QComboBox(parent);
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

} // namespace InkOptions
} // namespace ink
