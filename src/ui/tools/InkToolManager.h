#pragma once
// ToolManager: owns the active tool, the per-tool QActions and the routing of
// canvas events into the active tool. One QAction per tool carries the name,
// icon, shortcut, tooltip and checked state, so the toolbox buttons, the
// shortcut system and the options bar all read the same truth (the
// KoToolManager + KoToolAction concept, clean-room).
#include "ui/tools/InkToolFactory.h"
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

class QAction;
class QWidget;

namespace ink {

class Tool;
class ToolContext;
struct ToolEvent;

class ToolManager : public QObject {
    Q_OBJECT
public:
    explicit ToolManager(ToolContext *ctx, QObject *parent = nullptr);

    const std::vector<ToolFactory> &factories() const { return ToolRegistry::instance().factories(); }
    QAction *actionFor(const QString &id) const { return actions_.value(id, nullptr); }
    // Tool actions + global tool actions (size, swap/reset colors): add these
    // to the window so their shortcuts are live while the canvas has focus.
    QList<QAction *> windowActions() const { return windowActions_; }
    QAction *swapColorsAction() const { return swapAction_; }
    QAction *resetColorsAction() const { return resetAction_; }

    QString activeToolId() const { return activeId_; }
    Tool *activeTool() const;
    void setActiveTool(const QString &id);
    // The app's one manager, for tools that switch the active tool
    // (transform mode dropdown -> dedicated tool entries). Null in tests.
    static ToolManager *activeManager() { return activeManager_; }

    // event routing (canvas -> active tool)
    void press(const ToolEvent &e);
    void move(const ToolEvent &e);
    void release(const ToolEvent &e);
    void cancel();
    // Arrow keys: forwarded to the active tool, true when it consumed them.
    bool nudge(int dx, int dy, Qt::KeyboardModifiers mods);
    // Enter/Apply: forwarded to the active tool, true when it consumed it.
    bool commit();
    // Mirror the active region (move tool).
    void flipSelection(bool horizontal);

    // Options widget for a tool, created once and cached so slider state
    // survives tool switching.
    QWidget *optionsWidget(const QString &id, QWidget *parent);

signals:
    void activeToolChanged(const QString &id);
    void statusChanged(const QString &text);

private:
    void buildActions();
    Tool *ensureTool(const QString &id);

    static ToolManager *activeManager_;
    ToolContext *ctx_;
    QMap<QString, QAction *> actions_;
    QList<QAction *> windowActions_;
    QHash<QString, Tool *> tools_;
    QHash<QString, QWidget *> optionWidgets_;
    QString activeId_;
    QAction *swapAction_{nullptr};
    QAction *resetAction_{nullptr};
};

} // namespace ink
