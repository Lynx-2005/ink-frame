#include "ui/InkToolBox.h"
#include "ui/InkColorWells.h"
#include "ui/InkIcons.h"
#include "ui/InkTheme.h"
#include "ui/tools/InkToolFactory.h"
#include "ui/tools/InkToolManager.h"
#include <QAction>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace ink {

namespace {

constexpr int kButton = 30;
constexpr int kIcon = 18;
constexpr int kColumns = 2;
constexpr int kSpacing = 3;
constexpr int kMargin = 7;
constexpr int kExpandedWidth = kColumns * kButton + (kColumns - 1) * kSpacing + 2 * kMargin;
constexpr int kCollapsedWidth = 28;

} // namespace

ToolBox::ToolBox(ToolManager *tools, ToolContext *ctx, QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolBox"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header: the collapse toggle (the whole panel gets out of the way).
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("ToolBoxHeader"));
    header->setFixedHeight(24);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(2, 2, 2, 0);
    headerRow->setSpacing(0);
    headerRow->addStretch(1);
    collapse_ = new QToolButton(header);
    collapse_->setObjectName(QStringLiteral("ToolBoxCollapse"));
    collapse_->setFixedSize(22, 20);
    collapse_->setIconSize(QSize(14, 14));
    collapse_->setFocusPolicy(Qt::NoFocus);
    collapse_->setCursor(Qt::PointingHandCursor);
    connect(collapse_, &QToolButton::clicked, this, [this] { setCollapsed(!collapsed_); });
    headerRow->addWidget(collapse_);
    root->addWidget(header);

    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("ToolBoxBody"));
    auto *lay = new QVBoxLayout(body_);
    lay->setContentsMargins(kMargin, 2, kMargin, 8);
    lay->setSpacing(0);

    QString section;
    QGridLayout *grid = nullptr;
    int col = 0, row = 0;

    for (const ToolFactory &f : tools->factories()) {
        QAction *action = tools->actionFor(f.id);
        if (!action)
            continue;
        if (f.section != section) {
            if (!section.isEmpty()) {
                lay->addSpacing(8);
                auto *sep = new QFrame(body_);
                sep->setObjectName(QStringLiteral("ToolBoxSep"));
                sep->setFrameShape(QFrame::NoFrame);
                sep->setFixedHeight(1);
                lay->addWidget(sep);
                lay->addSpacing(8);
            }
            section = f.section;
            auto *group = new QWidget(body_);
            group->setObjectName(QStringLiteral("ToolBoxGroup"));
            grid = new QGridLayout(group);
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(kSpacing);
            grid->setVerticalSpacing(kSpacing);
            lay->addWidget(group);
            col = 0;
            row = 0;
        }
        auto *button = new QToolButton(body_);
        button->setObjectName(QStringLiteral("ToolBoxButton"));
        button->setFixedSize(kButton, kButton);
        button->setIconSize(QSize(kIcon, kIcon));
        button->setFocusPolicy(Qt::NoFocus);
        button->setDefaultAction(action);
        grid->addWidget(button, row, col, Qt::AlignLeft | Qt::AlignTop);
        if (++col == kColumns) {
            col = 0;
            ++row;
        }
    }

    lay->addStretch(1);
    lay->addSpacing(8);
    auto *sep = new QFrame(body_);
    sep->setObjectName(QStringLiteral("ToolBoxSep"));
    sep->setFrameShape(QFrame::NoFrame);
    sep->setFixedHeight(1);
    lay->addWidget(sep);
    lay->addSpacing(8);
    lay->addWidget(new ColorWells(ctx, tools, body_), 0, Qt::AlignHCenter);

    root->addWidget(body_, 1);
    root->addStretch(0); // keeps the header pinned to the top when collapsed

    const QSettings settings;
    setCollapsed(settings.value(QStringLiteral("toolbox/collapsed"), false).toBool());
}

void ToolBox::setCollapsed(bool collapsed) {
    collapsed_ = collapsed;
    if (body_)
        body_->setVisible(!collapsed);
    setFixedWidth(collapsed ? kCollapsedWidth : kExpandedWidth);
    if (collapse_) {
        collapse_->setIcon(InkIcons::get(collapsed ? IconId::ChevronRight : IconId::ChevronLeft,
                                         InkTheme::toolGlyph(), 14));
        collapse_->setToolTip(collapsed ? QStringLiteral("Expand the toolbox")
                                        : QStringLiteral("Collapse the toolbox"));
    }
    QSettings settings;
    settings.setValue(QStringLiteral("toolbox/collapsed"), collapsed);
}

} // namespace ink
