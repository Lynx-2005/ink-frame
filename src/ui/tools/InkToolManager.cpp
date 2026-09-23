#include "ui/tools/InkToolManager.h"
#include "ui/tools/InkTool.h"
#include "ui/tools/InkToolContext.h"
#include <QAction>
#include <QActionGroup>
#include <QWidget>
#include <algorithm>

namespace ink {

ToolManager::ToolManager(ToolContext *ctx, QObject *parent) : QObject(parent), ctx_(ctx) {
    registerBuiltinTools();
    buildActions();
    // Tools report feedback through the context ("Sampled #RRGGBB", size
    // changes); re-emit it as the manager's status so the options bar listens
    // to exactly one source.
    connect(ctx_, &ToolContext::statusChanged, this, &ToolManager::statusChanged);
}

void ToolManager::buildActions() {
    auto *group = new QActionGroup(this);
    group->setExclusive(true);

    for (const ToolFactory &f : factories()) {
        auto *a = new QAction(f.name, this);
        a->setCheckable(true);
        a->setIcon(InkIcons::get(f.icon, 18, false));
        if (!f.shortcut.isEmpty()) {
            a->setShortcut(f.shortcut);
            a->setToolTip(QStringLiteral("%1 (%2)").arg(f.name, f.shortcut.toString()));
        } else {
            a->setToolTip(f.name);
        }
        const QString id = f.id;
        connect(a, &QAction::triggered, this, [this, id] { setActiveTool(id); });
        group->addAction(a);
        actions_.insert(id, a);
        windowActions_.append(a);
    }

    auto *swap = new QAction(QStringLiteral("Swap colors"), this);
    swap->setShortcut(QKeySequence(Qt::Key_X));
    swap->setIcon(InkIcons::get(IconId::Swap, 16));
    swap->setToolTip(QStringLiteral("Swap foreground and background (X)"));
    connect(swap, &QAction::triggered, this, [this] { ctx_->swapColors(); });
    swapAction_ = swap;
    windowActions_.append(swap);

    auto *reset = new QAction(QStringLiteral("Reset colors"), this);
    reset->setShortcut(QKeySequence(Qt::Key_D));
    reset->setIcon(InkIcons::get(IconId::ResetColor, 16));
    reset->setToolTip(QStringLiteral("Reset to black and white (D)"));
    connect(reset, &QAction::triggered, this, [this] { ctx_->resetColors(); });
    resetAction_ = reset;
    windowActions_.append(reset);

    auto *sizeUp = new QAction(QStringLiteral("Increase brush size"), this);
    sizeUp->setShortcut(QKeySequence(Qt::Key_BracketRight));
    sizeUp->setToolTip(QStringLiteral("Increase brush size (])"));
    connect(sizeUp, &QAction::triggered, this, [this] {
        ctx_->setSize(std::min(256.0, ctx_->size() * 1.2));
        ctx_->setStatus(QStringLiteral("Brush size %1 px").arg(int(ctx_->size() + 0.5)));
    });
    windowActions_.append(sizeUp);

    auto *sizeDown = new QAction(QStringLiteral("Decrease brush size"), this);
    sizeDown->setShortcut(QKeySequence(Qt::Key_BracketLeft));
    sizeDown->setToolTip(QStringLiteral("Decrease brush size ([)"));
    connect(sizeDown, &QAction::triggered, this, [this] {
        ctx_->setSize(std::max(1.0, ctx_->size() / 1.2));
        ctx_->setStatus(QStringLiteral("Brush size %1 px").arg(int(ctx_->size() + 0.5)));
    });
    windowActions_.append(sizeDown);
}

Tool *ToolManager::ensureTool(const QString &id) {
    if (Tool *t = tools_.value(id, nullptr))
        return t;
    const ToolFactory *f = ToolRegistry::instance().find(id);
    if (!f || !f->create)
        return nullptr;
    Tool *t = f->create();
    t->setParent(this);
    t->activate(*ctx_);
    tools_.insert(id, t);
    return t;
}

void ToolManager::setActiveTool(const QString &id) {
    if (id == activeId_)
        return;
    if (Tool *old = activeTool())
        old->deactivate();
    Tool *t = ensureTool(id);
    if (!t)
        return;
    t->activate(*ctx_);
    activeId_ = id;
    if (QAction *a = actions_.value(id, nullptr))
        a->setChecked(true);
    emit activeToolChanged(id);
    emit statusChanged(t->statusHint());
}

Tool *ToolManager::activeTool() const { return tools_.value(activeId_, nullptr); }

void ToolManager::press(const ToolEvent &e) {
    if (Tool *t = activeTool())
        t->beginPrimary(e);
}

void ToolManager::move(const ToolEvent &e) {
    if (Tool *t = activeTool())
        t->continuePrimary(e);
}

void ToolManager::release(const ToolEvent &e) {
    if (Tool *t = activeTool())
        t->endPrimary(e);
}

void ToolManager::cancel() {
    if (Tool *t = activeTool())
        t->cancelPrimary();
}

QWidget *ToolManager::optionsWidget(const QString &id, QWidget *parent) {
    if (QWidget *w = optionWidgets_.value(id, nullptr))
        return w;
    Tool *t = ensureTool(id);
    if (!t)
        return nullptr;
    QWidget *w = t->createOptionsWidget(parent);
    if (!w)
        w = new QWidget(parent);
    w->setObjectName(QStringLiteral("ToolOptionsPage")); // transparent inside the bar
    w->setParent(parent);
    optionWidgets_.insert(id, w);
    return w;
}

} // namespace ink
