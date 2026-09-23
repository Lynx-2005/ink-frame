#include "ui/InkColorWells.h"
#include "ui/InkTheme.h"
#include "ui/tools/InkToolContext.h"
#include "ui/tools/InkToolManager.h"
#include <QAction>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace ink {

namespace {

// The swatch pair: background square behind, foreground square in front,
// each ringed with the panel color so the two never touch.
class SwatchPair : public QWidget {
public:
    explicit SwatchPair(ToolContext *ctx, QWidget *parent = nullptr)
        : QWidget(parent), ctx_(ctx) {
        setFixedSize(38, 38);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Foreground / background color"));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        // Background square behind, foreground square on top inside a
        // panel-colored ring: the ring is what keeps the two readable as two
        // swatches even when both colors are dark.
        p.setPen(Qt::NoPen);
        p.setBrush(ctx_->secondary());
        p.drawRect(bgRect());
        p.setBrush(InkTheme::chrome());
        p.drawRect(QRect(0, 0, 25, 25));
        p.setBrush(ctx_->primary());
        p.drawRect(fgRect());
        p.setPen(QPen(InkTheme::border(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(fgRect().adjusted(0, 0, -1, -1));
    }

    void mousePressEvent(QMouseEvent *e) override {
        const bool onFg = fgRect().contains(e->pos());
        const QColor current = onFg ? ctx_->primary() : ctx_->secondary();
        const QColor picked = QColorDialog::getColor(
            current, this, onFg ? QStringLiteral("Foreground color") : QStringLiteral("Background color"));
        if (!picked.isValid())
            return;
        if (onFg)
            ctx_->setPrimaryColor(picked);
        else
            ctx_->setSecondaryColor(picked);
    }

private:
    static QRect fgRect() { return QRect(2, 2, 21, 21); }
    static QRect bgRect() { return QRect(12, 12, 21, 21); }

    ToolContext *ctx_;
};

QToolButton *smallButton(QAction *action, QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("WellBtn"));
    b->setFixedSize(20, 20);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    if (action)
        b->setDefaultAction(action);
    return b;
}

} // namespace

ColorWells::ColorWells(ToolContext *ctx, ToolManager *tools, QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ColorWells"));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(1, 0, 0, 0);
    row->setSpacing(4);

    auto *swatches = new SwatchPair(ctx, this);
    connect(ctx, &ToolContext::colorsChanged, swatches, [swatches] { swatches->update(); });
    row->addWidget(swatches);

    auto *buttons = new QVBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(2);
    buttons->addWidget(smallButton(tools->swapColorsAction(), this));
    buttons->addWidget(smallButton(tools->resetColorsAction(), this));
    buttons->addStretch(1);
    row->addLayout(buttons);
    row->addStretch(1);
}

} // namespace ink
