#include "ui/tools/InkColorPickerTool.h"
#include "core/InkPaintDevice.h"
#include "ui/InkDocument.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QHBoxLayout>
#include <QLabel>
#include <cmath>

namespace ink {

void ColorPickerTool::activate(ToolContext &ctx) { Tool::activate(ctx); }

void ColorPickerTool::beginPrimary(const ToolEvent &e) { sample(e); }

void ColorPickerTool::continuePrimary(const ToolEvent &e) { sample(e); }

void ColorPickerTool::sample(const ToolEvent &e) {
    if (!ctx_)
        return;
    const Image &image = ctx_->doc()->image();
    const int x = int(std::floor(e.docPos.x));
    const int y = int(std::floor(e.docPos.y));
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height())
        return;
    PaintDevice projection;
    image.project(ctx_->doc()->currentFrame(), projection);
    const PixelRGBA8 px = projection.pixel(x, y);
    const QColor picked(px.r, px.g, px.b);
    if (e.mods & Qt::ControlModifier)
        ctx_->setSecondaryColor(picked);
    else
        ctx_->setPrimaryColor(picked);
    ctx_->setStatus(QStringLiteral("Sampled %1").arg(picked.name(QColor::HexRgb).toUpper()));
}

QWidget *ColorPickerTool::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    row->addWidget(InkOptions::caption(QStringLiteral("SOURCE"), page));
    auto *merged = InkOptions::value(QStringLiteral("Merged image"), page, 0);
    row->addWidget(merged);
    row->addWidget(InkOptions::separator(page));
    auto *hint = new QLabel(QStringLiteral("Ctrl+click samples the background color"), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);
    row->addStretch(1);
    return page;
}

} // namespace ink
