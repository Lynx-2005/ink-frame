#include "ui/InkTopBar.h"
#include "ui/InkIcons.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace ink {

TopBar::TopBar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TopBar"));
    setFixedHeight(44);
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 4, 12, 4);
    lay->setSpacing(8);

    auto *logo = new QLabel(QStringLiteral("Kōsei"), this);
    logo->setObjectName(QStringLiteral("Logo"));
    lay->addWidget(logo);

    projBox_ = new QComboBox(this);
    projBox_->setEditable(true);
    projBox_->addItem(QStringLiteral("Project 01"));
    projBox_->setMinimumWidth(150);
    lay->addWidget(projBox_);
    lay->addStretch(1);

    auto *undo = new QToolButton(this);
    undo->setIcon(InkIcons::get(IconId::Undo, 18));
    undo->setToolTip(QStringLiteral("Undo (Ctrl+Z)"));
    auto *redo = new QToolButton(this);
    redo->setIcon(InkIcons::get(IconId::Redo, 18));
    redo->setToolTip(QStringLiteral("Redo (Ctrl+Y)"));
    connect(undo, &QToolButton::clicked, this, &TopBar::undoRequested);
    connect(redo, &QToolButton::clicked, this, &TopBar::redoRequested);
    lay->addWidget(undo);
    lay->addWidget(redo);

    zoomBox_ = new QComboBox(this);
    zoomBox_->setEditable(false);
    for (const char *z : {"25%", "50%", "100%", "200%", "400%"}) zoomBox_->addItem(QString::fromLatin1(z));
    zoomBox_->setCurrentText(QStringLiteral("100%"));
    connect(zoomBox_, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        QString n = t;
        n.remove('%');
        bool ok = false;
        double v = n.toDouble(&ok);
        if (ok) emit zoomPicked(v / 100.0);
    });
    zoomBox_->setToolTip(QStringLiteral("Canvas zoom"));
    lay->addWidget(zoomBox_);

    auto *menu = new QToolButton(this);
    menu->setIcon(InkIcons::get(IconId::Menu, 18));
    menu->setToolTip(QStringLiteral("Show menus"));    connect(menu, &QToolButton::clicked, this, &TopBar::menuRequested);
    lay->addWidget(menu);
}

void TopBar::setZoom(double z) {
    int pct = int(z * 100.0 + 0.5);
    zoomBox_->blockSignals(true);
    zoomBox_->setCurrentText(QString::number(pct) + QStringLiteral("%"));
    zoomBox_->blockSignals(false);
}

} // namespace ink
