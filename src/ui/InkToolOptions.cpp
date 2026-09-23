#include "ui/InkToolOptions.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QStackedWidget>

namespace ink {

static QWidget *mkRow(const QString &label, QSlider *slider, QLabel *val) {
    auto *w = new QWidget;
    auto *l = new QHBoxLayout(w);
    l->setContentsMargins(8, 2, 8, 2);
    l->setSpacing(8);
    auto *t = new QLabel(label, w);
    t->setObjectName(QStringLiteral("Section"));
    l->addWidget(t);
    slider->setRange(1, 128);
    slider->setMaximumWidth(160);
    l->addWidget(slider);
    l->addWidget(val);
    l->addStretch(1);
    return w;
}

ToolOptions::ToolOptions(QWidget *parent) : QWidget(parent) {
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    l->addWidget(stack_);

    sizeSlider_ = new QSlider(Qt::Horizontal, this);
    sizeLabel_ = new QLabel(QStringLiteral("8 px"), this);
    sizeLabel_->setMinimumWidth(48);
    brushPage_ = mkRow(QStringLiteral("BRUSH"), sizeSlider_, sizeLabel_);
    stack_->addWidget(brushPage_);
    connect(sizeSlider_, &QSlider::valueChanged, this, [this](int v) {
        sizeLabel_->setText(QString::number(v) + QStringLiteral(" px"));
        emit brushSizeChanged(double(v));
    });

    auto *shapeW = new QWidget(this);
    auto *sl = new QHBoxLayout(shapeW);
    sl->setContentsMargins(8, 2, 8, 2);
    sl->setSpacing(8);
    auto *mt = new QLabel(QStringLiteral("SHAPE"), shapeW);
    mt->setObjectName(QStringLiteral("Section"));
    sl->addWidget(mt);
    modeBox_ = new QComboBox(shapeW);
    modeBox_->addItem(QStringLiteral("Line"), int(ShapeMode::Line));
    modeBox_->addItem(QStringLiteral("Rectangle"), int(ShapeMode::Rect));
    modeBox_->addItem(QStringLiteral("Ellipse"), int(ShapeMode::Ellipse));
    sl->addWidget(modeBox_);
    shapeSizeSlider_ = new QSlider(Qt::Horizontal, shapeW);
    shapeSizeSlider_->setRange(1, 128);
    shapeSizeSlider_->setMaximumWidth(140);
    shapeSizeLabel_ = new QLabel(QStringLiteral("8 px"), shapeW);
    shapeSizeLabel_->setMinimumWidth(48);
    sl->addWidget(shapeSizeSlider_);
    sl->addWidget(shapeSizeLabel_);
    sl->addStretch(1);
    shapePage_ = shapeW;
    stack_->addWidget(shapePage_);
    connect(modeBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        emit shapeModeChanged(ShapeMode(modeBox_->itemData(i).toInt()));
    });
    connect(shapeSizeSlider_, &QSlider::valueChanged, this, [this](int v) {
        shapeSizeLabel_->setText(QString::number(v) + QStringLiteral(" px"));
        sizeSlider_->setValue(v); // keep both sliders in sync
        emit brushSizeChanged(double(v));
    });

    auto *pickW = new QWidget(this);
    auto *pkl = new QHBoxLayout(pickW);
    pkl->setContentsMargins(8, 2, 8, 2);
    auto *hint = new QLabel(QStringLiteral("Click the canvas to sample a color into the primary well."), pickW);
    hint->setObjectName(QStringLiteral("Hint"));
    pkl->addWidget(hint);
    pkl->addStretch(1);
    pickerPage_ = pickW;
    stack_->addWidget(pickerPage_);

    setTool(ToolId::Brush);
    setFixedHeight(34);
}

void ToolOptions::setTool(ToolId t) {
    if (t == ToolId::Shape) stack_->setCurrentWidget(shapePage_);
    else if (t == ToolId::Picker) stack_->setCurrentWidget(pickerPage_);
    else stack_->setCurrentWidget(brushPage_);
}

void ToolOptions::setBrushSize(double px) {
    sizeSlider_->blockSignals(true);
    shapeSizeSlider_->blockSignals(true);
    sizeSlider_->setValue(int(px));
    shapeSizeSlider_->setValue(int(px));
    sizeLabel_->setText(QString::number(int(px)) + QStringLiteral(" px"));
    shapeSizeLabel_->setText(QString::number(int(px)) + QStringLiteral(" px"));
    sizeSlider_->blockSignals(false);
    shapeSizeSlider_->blockSignals(false);
}

} // namespace ink
