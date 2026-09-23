#include "ui/InkToolBox.h"
#include "ui/InkColorWells.h"
#include "ui/tools/InkToolFactory.h"
#include "ui/tools/InkToolManager.h"
#include <QAction>
#include <QFrame>
#include <QGridLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace ink {

namespace {

constexpr int kButton = 30;
constexpr int kIcon = 18;
constexpr int kColumns = 2;
constexpr int kSpacing = 3;

} // namespace

ToolBox::ToolBox(ToolManager *tools, ToolContext *ctx, QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolBox"));
    setFixedWidth(2 * kButton + kSpacing + 14);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(7, 8, 7, 8);
    root->setSpacing(0);

    QString section;
    QGridLayout *grid = nullptr;
    int col = 0, row = 0;

    for (const ToolFactory &f : tools->factories()) {
        QAction *action = tools->actionFor(f.id);
        if (!action)
            continue;
        if (f.section != section) {
            if (!section.isEmpty()) {
                root->addSpacing(8);
                auto *sep = new QFrame(this);
                sep->setObjectName(QStringLiteral("ToolBoxSep"));
                sep->setFrameShape(QFrame::NoFrame);
                sep->setFixedHeight(1);
                root->addWidget(sep);
                root->addSpacing(8);
            }
            section = f.section;
            auto *group = new QWidget(this);
            group->setObjectName(QStringLiteral("ToolBoxGroup"));
            grid = new QGridLayout(group);
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(kSpacing);
            grid->setVerticalSpacing(kSpacing);
            root->addWidget(group);
            col = 0;
            row = 0;
        }
        auto *button = new QToolButton(this);
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

    root->addStretch(1);
    root->addSpacing(8);
    auto *sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("ToolBoxSep"));
    sep->setFrameShape(QFrame::NoFrame);
    sep->setFixedHeight(1);
    root->addWidget(sep);
    root->addSpacing(8);
    root->addWidget(new ColorWells(ctx, tools, this), 0, Qt::AlignHCenter);
}

} // namespace ink
