#include "ui/InkTopBar.h"
#include "ui/InkIcons.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QToolButton>
#include <cmath>

namespace ink {

TopBar::TopBar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TopBar"));
    setFixedHeight(44);
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(12, 4, 12, 4);
    lay->setSpacing(8);

    projBox_ = new QComboBox(this);
    projBox_->setEditable(true);
    projBox_->addItem(QStringLiteral("Project 01"));
    projBox_->setMinimumWidth(150);
    lay->addWidget(projBox_);

    // History sits with the project, not stranded mid-bar: the old stretch
    // between the project box and the undo cluster left most of the banner
    // visibly empty.
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

    lay->addStretch(1);

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
    zoomBox_->setToolTip(QStringLiteral("Canvas zoom · 1/3 zoom, 2 = 100%, Ctrl+0 fit"));
    lay->addWidget(zoomBox_);

    // Canvas rotation: the view turns, the pixels never do (4/6 nudge it, 5
    // clears it). Editable so the readout can show any angle the keys produce.
    rotBox_ = new QComboBox(this);
    rotBox_->setEditable(true);
    for (const char *r : {"0°", "15°", "30°", "45°", "90°", "135°", "180°"})
        rotBox_->addItem(QString::fromUtf8(r));
    rotBox_->setCurrentText(QStringLiteral("0°"));
    connect(rotBox_, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        QString n = t;
        n.remove(QChar(0x00b0));
        bool ok = false;
        const double v = n.toDouble(&ok);
        if (ok) emit rotatePicked(v);
    });
    rotBox_->setToolTip(QStringLiteral("Canvas rotation · 4/6 rotate, 5 resets"));
    lay->addWidget(rotBox_);

    // Layer panel toggle: three dots, checked while the panel is open. The
    // File/Edit/View menu bar this button used to reveal is gone — the layer
    // panel is the right-side surface now.
    layersBtn_ = new QToolButton(this);
    layersBtn_->setIcon(InkIcons::get(IconId::More, 18));
    layersBtn_->setToolTip(QStringLiteral("Layer panel (F7)"));
    layersBtn_->setCheckable(true);
    connect(layersBtn_, &QToolButton::toggled, this, &TopBar::layersToggled);
    lay->addWidget(layersBtn_);
}

void TopBar::setLayersChecked(bool on) {
    if (!layersBtn_ || layersBtn_->isChecked() == on) return;
    layersBtn_->blockSignals(true);
    layersBtn_->setChecked(on);
    layersBtn_->blockSignals(false);
}

void TopBar::setZoom(double z) {
    int pct = int(z * 100.0 + 0.5);
    zoomBox_->blockSignals(true);
    zoomBox_->setCurrentText(QString::number(pct) + QStringLiteral("%"));
    zoomBox_->blockSignals(false);
}

void TopBar::setRotation(double degrees) {
    // Wrap to (-180, 180] so the readout never grows without bound.
    double d = std::fmod(degrees, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    rotBox_->blockSignals(true);
    rotBox_->setCurrentText(QStringLiteral("%1°").arg(d, 0, 'g', 4));
    rotBox_->blockSignals(false);
}

} // namespace ink
