#include "ui/tools/InkFillTool.h"
#include "core/InkFloodFill.h"
#include "core/InkPaintDevice.h"
#include "ui/InkDocument.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <cmath>

namespace ink {

void FillTool::activate(ToolContext &ctx) { Tool::activate(ctx); }

void FillTool::beginPrimary(const ToolEvent &e) {
    if (!ctx_)
        return;
    PaintDevice *target = ctx_->editableDevice();
    if (!target) {
        ctx_->setStatus(ctx_->blockedHint());
        return;
    }
    const int w = ctx_->doc()->image().width();
    const int h = ctx_->doc()->image().height();
    const int x = int(std::floor(e.docPos.x));
    const int y = int(std::floor(e.docPos.y));
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;

    // Merged sampling reads what the user sees; it is projected into a
    // temporary that outlives the call.
    PaintDevice projection;
    const PaintDevice *source = target;
    if (sampleMerged_) {
        ctx_->doc()->image().project(ctx_->doc()->currentFrame(), projection);
        source = &projection;
    }

    const QColor c = ctx_->primary();
    const PixelRGBA8 color{uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};

    FloodFillParams params;
    params.tolerance = tolerance_;
    params.contiguous = (e.mods & Qt::ShiftModifier) ? false : contiguous_;
    params.clip = ctx_->selectionClip();

    TileMemento memento;
    const Rect dirty =
        floodFill(*target, *source, Rect{0, 0, w, h}, x, y, color, params, &memento);
    if (memento.empty()) {
        ctx_->setStatus(QStringLiteral("Nothing to fill"));
        return;
    }
    ctx_->markDirty(dirty);
    ctx_->pushStrokeCommand(QStringLiteral("Fill"), target, std::move(memento));
    ctx_->setStatus(QStringLiteral("Filled %1 × %2 px").arg(dirty.w).arg(dirty.h));
}

QWidget *FillTool::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    auto *source = InkOptions::combo(
        {QStringLiteral("Merged image"), QStringLiteral("Current layer")}, page, 118);
    source->setCurrentIndex(sampleMerged_ ? 0 : 1);
    connect(source, &QComboBox::currentIndexChanged, page,
            [this](int i) { sampleMerged_ = (i == 0); });
    InkOptions::addField(row, QStringLiteral("SOURCE"), source);

    row->addWidget(InkOptions::separator(page));
    auto *tolerance = InkOptions::slider(0, 100, tolerance_ * 100 / 255, page, 84);
    auto *toleranceVal =
        InkOptions::value(QStringLiteral("%1%").arg(tolerance->value()), page, 36);
    InkOptions::addField(row, QStringLiteral("TOLERANCE"), tolerance);
    row->addWidget(toleranceVal);
    connect(tolerance, &QSlider::valueChanged, page, [this, toleranceVal](int v) {
        tolerance_ = int(std::lround(v * 2.55));
        toleranceVal->setText(QStringLiteral("%1%").arg(v));
    });

    row->addWidget(InkOptions::separator(page));
    auto *contiguous = InkOptions::toggle(QStringLiteral("Contiguous"), contiguous_, page);
    row->addWidget(contiguous);
    connect(contiguous, &QToolButton::toggled, page, [this](bool on) { contiguous_ = on; });

    row->addWidget(InkOptions::separator(page));
    auto *hint = new QLabel(QStringLiteral("Shift+click fills every similar pixel"), page);
    hint->setObjectName(QStringLiteral("OptHint"));
    row->addWidget(hint);
    row->addStretch(1);
    return page;
}

} // namespace ink
