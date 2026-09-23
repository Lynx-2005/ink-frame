#include "ui/InkToolRail.h"
#include "ui/InkIcons.h"
#include <QColorDialog>
#include <QToolButton>
#include <QVBoxLayout>

namespace ink {

ToolRail::ToolRail(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolRail"));
    setFixedWidth(58);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(7, 8, 7, 8);
    lay->setSpacing(5);
    struct Item { ToolId id; IconId icon; const char *tip; const char *key; bool soon; };
    const Item items[] = {
        {ToolId::Brush, IconId::Brush, "Brush", "B", false},
        {ToolId::Eraser, IconId::Eraser, "Eraser", "E", false},
        {ToolId::Shape, IconId::Shape, "Shape (line/rect/ellipse)", "U", false},
        {ToolId::Picker, IconId::Picker, "Color picker", "I", false},
        {ToolId::Text, IconId::Text, "Text — coming soon", "", true},
        {ToolId::Note, IconId::Note, "Note — coming soon", "", true},
        {ToolId::Select, IconId::Select, "Select — coming soon", "", true},
    };
    for (auto &it : items) {
        auto *b = new QToolButton(this);
        b->setObjectName(QStringLiteral("RailBtn"));
        b->setIcon(InkIcons::get(it.icon, 22));
        b->setCheckable(true);
        b->setMinimumSize(42, 42);
        QString tip = QString::fromLatin1(it.tip);
        if (!it.soon && it.key[0]) {
            tip += QStringLiteral(" (%1)").arg(QString::fromLatin1(it.key));
            b->setShortcut(QKeySequence(QString::fromLatin1(it.key)));
        }
        b->setToolTip(tip);
        if (it.soon) {
            b->setEnabled(false); // honest placeholder: no dead clicks
        } else {
            connect(b, &QToolButton::clicked, this, [this, id = it.id] { setTool(id); });
        }
        b->setProperty("toolId", int(it.id));
        lay->addWidget(b);
        btns_.append(b);
    }
    lay->addStretch(1);
    wellA_ = new QToolButton(this);
    wellB_ = new QToolButton(this);
    for (auto *w : {wellA_, wellB_}) {
        w->setObjectName(QStringLiteral("Well"));
        w->setMinimumSize(36, 36);
        lay->addWidget(w, 0, Qt::AlignHCenter);
    }
    wellA_->setToolTip(QStringLiteral("Primary color (click to pick)"));
    wellB_->setToolTip(QStringLiteral("Secondary color (click to pick)"));
    connect(wellA_, &QToolButton::clicked, this, &ToolRail::pickA);
    connect(wellB_, &QToolButton::clicked, this, &ToolRail::pickB);
    setTool(ToolId::Brush);
    refreshWells();
}

void ToolRail::setTool(ToolId t) {
    if (t == ToolId::Text || t == ToolId::Note || t == ToolId::Select) return; // soon slots
    tool_ = t;
    for (auto *b : btns_) b->setChecked(b->property("toolId").toInt() == int(t));
    emit toolChanged(t);
}

void ToolRail::setPrimaryColor(const QColor &c) {
    if (!c.isValid() || c == colA_) return;
    colA_ = c;
    refreshWells();
    emit colorsChanged(colA_, colB_);
}

void ToolRail::refreshWells() {    wellA_->setStyleSheet(QStringLiteral("QToolButton#Well { background: %1; border: 2px solid #E08A94; }")
                              .arg(colA_.name()));
    wellB_->setStyleSheet(QStringLiteral("QToolButton#Well { background: %1; border: 2px solid #45464E; }")
                              .arg(colB_.name()));
}

void ToolRail::pickA() {
    QColor c = QColorDialog::getColor(colA_, this, QStringLiteral("Primary color"));
    if (c.isValid()) { colA_ = c; refreshWells(); emit colorsChanged(colA_, colB_); }
}
void ToolRail::pickB() {
    QColor c = QColorDialog::getColor(colB_, this, QStringLiteral("Secondary color"));
    if (c.isValid()) { colB_ = c; refreshWells(); emit colorsChanged(colA_, colB_); }
}

} // namespace ink
