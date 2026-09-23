#include "ui/tools/InkPaintTool.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolContext.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <cmath>

namespace ink {

namespace {

struct PaintWidgets {
    QComboBox *preset{nullptr};
    QSlider *size{nullptr};
    QSlider *opacity{nullptr};
    QSlider *hardness{nullptr};
    QSlider *spacing{nullptr};
    QSlider *smooth{nullptr};
    QLabel *sizeVal{nullptr};
    QLabel *opacityVal{nullptr};
    QLabel *hardnessVal{nullptr};
    QLabel *spacingVal{nullptr};
    QLabel *smoothVal{nullptr};
    QToolButton *pSize{nullptr};
    QToolButton *pOpacity{nullptr};
};

} // namespace

void PaintToolBase::activate(ToolContext &ctx) {
    Tool::activate(ctx);
    hasBuilt_ = false; // next stroke rebuilds the op off fresh settings
}

void PaintToolBase::deactivate() { cancelPrimary(); }

QCursor PaintToolBase::cursor() const { return QCursor(Qt::BlankCursor); }

void PaintToolBase::paintOverlay(QPainter &p, const OverlayInfo &info) const {
    if (!ctx_ || !info.hover)
        return;
    const double radius = std::max(1.0, ctx_->size() * 0.5);
    const double halo = 1.5 / std::max(0.05, info.zoom);
    const QPointF c(info.hoverPos.x, info.hoverPos.y);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 210), 0));
    p.drawEllipse(c, radius, radius);
    p.setPen(QPen(QColor(0, 0, 0, 170), 0));
    p.drawEllipse(c, radius + halo, radius + halo);
}

bool PaintToolBase::opMatches(const BrushPreset &p) const {
    if (!hasBuilt_)
        return false;
    return built_.tip.diameter == p.tip.diameter && built_.tip.hardness == p.tip.hardness &&
           built_.tip.opacity == p.tip.opacity && built_.tip.spacing == p.tip.spacing &&
           built_.dynamics.sizePressure.enabled == p.dynamics.sizePressure.enabled &&
           built_.dynamics.sizePressure.amount == p.dynamics.sizePressure.amount &&
           built_.dynamics.opacityPressure.enabled == p.dynamics.opacityPressure.enabled &&
           built_.dynamics.opacityPressure.amount == p.dynamics.opacityPressure.amount &&
           built_.dynamics.smoothing.mode == p.dynamics.smoothing.mode &&
           built_.dynamics.smoothing.amount == p.dynamics.smoothing.amount;
}

void PaintToolBase::rebuildOp() {
    op_ = makeOp(strokePreset_);
    built_ = strokePreset_;
    hasBuilt_ = true;
}

void PaintToolBase::beginPrimary(const ToolEvent &e) {
    if (stroking_ || !ctx_)
        return;
    device_ = ctx_->editableDevice();
    if (!device_) {
        ctx_->setStatus(QStringLiteral("Layer is locked or not paintable"));
        return;
    }
    strokePreset_ = ctx_->preset();
    const QColor c = ctx_->primary();
    strokeColor_ = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), 255};
    memento_ = TileMemento{};
    opDirty_ = Rect{};
    if (!op_ || !opMatches(strokePreset_))
        rebuildOp();
    PaintOpContext opctx{device_, strokeColor_, CompositeOp::Normal, &memento_, &opDirty_,
                         ctx_->selectionClip()};
    op_->beginStroke(opctx);
    stroking_ = true;
    sendEvent(e);
}

void PaintToolBase::sendEvent(const ToolEvent &e) {
    StrokeEvent ev;
    ev.state.x = float(e.docPos.x);
    ev.state.y = float(e.docPos.y);
    ev.state.pressure = e.pressure;
    ev.state.tiltX = e.tiltX;
    ev.state.tiltY = e.tiltY;
    ev.timeSec = e.timeSec;
    opDirty_ = Rect{};
    op_->addEvent(ev);
    if (!opDirty_.isEmpty())
        ctx_->markDirty(opDirty_);
}

void PaintToolBase::continuePrimary(const ToolEvent &e) {
    if (!stroking_)
        return;
    sendEvent(e);
}

void PaintToolBase::endPrimary(const ToolEvent &) {
    if (!stroking_)
        return;
    op_->endStroke();
    stroking_ = false;
    if (device_ && !memento_.empty())
        ctx_->pushStrokeCommand(strokeName(), device_, std::move(memento_));
    memento_ = TileMemento{};
    device_ = nullptr;
}

void PaintToolBase::cancelPrimary() {
    if (!stroking_)
        return;
    op_->endStroke();
    stroking_ = false;
    if (device_ && !memento_.empty())
        device_->store().restore(memento_);
    memento_ = TileMemento{};
    device_ = nullptr;
    ctx_->requestRebuild();
}

QWidget *PaintToolBase::createOptionsWidget(QWidget *parent) {
    auto *page = new QWidget(parent);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    PaintWidgets w;
    const BrushPreset &p0 = ctx_->preset();

    const QStringList presets = presetIds();
    if (presets.size() > 1) {
        w.preset = InkOptions::combo({}, page, 104);
        for (const QString &id : presets) {
            const BrushPreset *p = ctx_->resources().find(id.toStdString());
            w.preset->addItem(p ? QString::fromStdString(p->name) : id, id);
        }
        row->addWidget(w.preset);
        row->addWidget(InkOptions::separator(page));
        connect(w.preset, &QComboBox::currentIndexChanged, page, [this, w](int index) {
            const QString id = w.preset->itemData(index).toString();
            if (!id.isEmpty())
                ctx_->setPresetId(id);
        });
    }

    w.size = InkOptions::slider(1, 256, int(p0.tip.diameter + 0.5f), page, 90);
    w.sizeVal = InkOptions::value(QStringLiteral("%1 px").arg(w.size->value()), page);
    InkOptions::addField(row, QStringLiteral("SIZE"), w.size);
    row->addWidget(w.sizeVal);

    w.opacity = InkOptions::slider(1, 100, int(p0.tip.opacity * 100.0f + 0.5f), page, 70);
    w.opacityVal = InkOptions::value(QStringLiteral("%1%").arg(w.opacity->value()), page, 36);
    InkOptions::addField(row, QStringLiteral("OPACITY"), w.opacity);
    row->addWidget(w.opacityVal);

    w.hardness = InkOptions::slider(0, 100, int(p0.tip.hardness * 100.0f + 0.5f), page, 70);
    w.hardnessVal = InkOptions::value(QStringLiteral("%1%").arg(w.hardness->value()), page, 36);
    InkOptions::addField(row, QStringLiteral("HARDNESS"), w.hardness);
    row->addWidget(w.hardnessVal);

    w.spacing = InkOptions::slider(1, 100, int(p0.tip.spacing * 100.0f + 0.5f), page, 70);
    w.spacingVal = InkOptions::value(QStringLiteral("%1%").arg(w.spacing->value()), page, 36);
    InkOptions::addField(row, QStringLiteral("SPACING"), w.spacing);
    row->addWidget(w.spacingVal);

    row->addWidget(InkOptions::separator(page));
    w.pSize = InkOptions::toggle(QStringLiteral("Pressure → size"),
                                 p0.dynamics.sizePressure.enabled, page);
    w.pOpacity = InkOptions::toggle(QStringLiteral("Pressure → opacity"),
                                    p0.dynamics.opacityPressure.enabled, page);
    row->addWidget(w.pSize);
    row->addWidget(w.pOpacity);

    row->addWidget(InkOptions::separator(page));
    w.smooth = InkOptions::slider(0, 100, int(p0.dynamics.smoothing.amount * 100.0f + 0.5f), page, 70);
    w.smoothVal = InkOptions::value(QStringLiteral("%1%").arg(w.smooth->value()), page, 36);
    InkOptions::addField(row, QStringLiteral("STABILIZER"), w.smooth);
    row->addWidget(w.smoothVal);
    row->addStretch(1);

    connect(w.size, &QSlider::valueChanged, page, [this, w](int v) {
        w.sizeVal->setText(QStringLiteral("%1 px").arg(v));
        ctx_->setSize(v);
    });
    connect(w.opacity, &QSlider::valueChanged, page, [this, w](int v) {
        w.opacityVal->setText(QStringLiteral("%1%").arg(v));
        ctx_->setOpacity(v);
    });
    connect(w.hardness, &QSlider::valueChanged, page, [this, w](int v) {
        w.hardnessVal->setText(QStringLiteral("%1%").arg(v));
        ctx_->setHardness(v);
    });
    connect(w.spacing, &QSlider::valueChanged, page, [this, w](int v) {
        w.spacingVal->setText(QStringLiteral("%1%").arg(v));
        ctx_->setSpacing(v);
    });
    connect(w.smooth, &QSlider::valueChanged, page, [this, w](int v) {
        w.smoothVal->setText(QStringLiteral("%1%").arg(v));
        ctx_->setSmoothing(v);
    });
    connect(w.pSize, &QToolButton::toggled, page, [this](bool on) { ctx_->setPressureSize(on); });
    connect(w.pOpacity, &QToolButton::toggled, page, [this](bool on) { ctx_->setPressureOpacity(on); });

    // Pull changes that came from elsewhere ([ ] size keys, preset switch,
    // picker, another tool's page) back into the controls.
    connect(ctx_, &ToolContext::settingsChanged, page, [this, w] {
        const BrushPreset &p = ctx_->preset();
        const QSignalBlocker b1(w.size), b2(w.opacity), b3(w.hardness), b4(w.spacing), b5(w.smooth);
        if (w.preset) {
            const QSignalBlocker b6(w.preset);
            const int index = w.preset->findData(QString::fromStdString(p.id));
            if (index >= 0)
                w.preset->setCurrentIndex(index);
        }
        w.size->setValue(int(p.tip.diameter + 0.5f));
        w.opacity->setValue(int(p.tip.opacity * 100.0f + 0.5f));
        w.hardness->setValue(int(p.tip.hardness * 100.0f + 0.5f));
        w.spacing->setValue(int(p.tip.spacing * 100.0f + 0.5f));
        w.smooth->setValue(int(p.dynamics.smoothing.amount * 100.0f + 0.5f));
        {
            const QSignalBlocker c1(w.pSize), c2(w.pOpacity);
            w.pSize->setChecked(p.dynamics.sizePressure.enabled);
            w.pOpacity->setChecked(p.dynamics.opacityPressure.enabled);
        }
        w.sizeVal->setText(QStringLiteral("%1 px").arg(w.size->value()));
        w.opacityVal->setText(QStringLiteral("%1%").arg(w.opacity->value()));
        w.hardnessVal->setText(QStringLiteral("%1%").arg(w.hardness->value()));
        w.spacingVal->setText(QStringLiteral("%1%").arg(w.spacing->value()));
        w.smoothVal->setText(QStringLiteral("%1%").arg(w.smooth->value()));
    });

    return page;
}

} // namespace ink
