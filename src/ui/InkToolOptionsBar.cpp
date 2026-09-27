#include "ui/InkToolOptionsBar.h"
#include "ui/InkIcons.h"
#include "ui/InkOptionWidgets.h"
#include "ui/tools/InkToolFactory.h"
#include "ui/tools/InkToolManager.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QStackedWidget>

namespace ink {

void ElidedLabel::setFullText(const QString &t) {
    full_ = t;
    updateElided();
}

void ElidedLabel::resizeEvent(QResizeEvent *e) {
    QLabel::resizeEvent(e);
    updateElided();
}

void ElidedLabel::updateElided() {
    QLabel::setText(fontMetrics().elidedText(full_, Qt::ElideRight, std::max(0, width() - 4)));
}

ToolOptionsBar::ToolOptionsBar(ToolManager *tools, QWidget *parent) : QWidget(parent), tools_(tools) {
    setObjectName(QStringLiteral("ToolOptionsBar"));
    setFixedHeight(34);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(8);

    icon_ = new QLabel(this);
    icon_->setFixedSize(16, 16);
    row->addWidget(icon_);

    name_ = new QLabel(this);
    name_->setObjectName(QStringLiteral("ToolName"));
    name_->setMinimumWidth(58);
    row->addWidget(name_);

    row->addWidget(InkOptions::separator(this));

    stack_ = new QStackedWidget(this);
    row->addWidget(stack_, 1);

    auto *status = new ElidedLabel(this);
    status->setObjectName(QStringLiteral("OptHint"));
    status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    status->setMinimumWidth(80);
    row->addWidget(status, 2);
    status_ = status;

    connect(tools_, &ToolManager::activeToolChanged, this, [this](const QString &id) { showTool(id); });
    connect(tools_, &ToolManager::statusChanged, this, &ToolOptionsBar::setStatus);

    if (!tools_->activeToolId().isEmpty())
        showTool(tools_->activeToolId());
}

void ToolOptionsBar::showTool(const QString &id) {
    const ToolFactory *f = ToolRegistry::instance().find(id);
    if (!f)
        return;
    icon_->setPixmap(InkIcons::get(f->icon, 16, false).pixmap(16, 16));
    name_->setText(f->name);
    if (QWidget *page = tools_->optionsWidget(id, stack_)) {
        if (stack_->indexOf(page) < 0)
            stack_->addWidget(page);
        // Size to the CURRENT page only: QStackedWidget otherwise reports the
        // max of all pages (hidden included), so one wide tool would widen the
        // bar — and the whole main window — forever.
        for (int i = 0; i < stack_->count(); ++i)
            if (QWidget *w = stack_->widget(i))
                w->setSizePolicy(i == stack_->indexOf(page) ? QSizePolicy::Preferred
                                                            : QSizePolicy::Ignored,
                                 QSizePolicy::Preferred);
        stack_->setCurrentWidget(page);
        stack_->updateGeometry();
    }
    setStatus({});
}

void ToolOptionsBar::setStatus(const QString &text) { status_->setFullText(text); }

} // namespace ink
