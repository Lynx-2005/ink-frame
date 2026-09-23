#pragma once
// Tool factories + registry. A factory carries everything the toolbox and
// the shortcut system need (id, name, icon, section, shortcut, order), and
// knows how to build its tool instance. Clean-room take on Krita's
// KoToolFactoryBase + KoToolRegistry concept.
#include "ui/InkIcons.h"
#include <QKeySequence>
#include <QString>
#include <functional>
#include <vector>

namespace ink {

class Tool;

// Section ids carry a numeric prefix so plain string sorting gives the
// toolbox order (same trick Krita uses for its ToolBoxSection names).
namespace ToolSections {
inline QString paint() { return QStringLiteral("0 Paint"); }
inline QString fill() { return QStringLiteral("1 Fill"); }
inline QString shape() { return QStringLiteral("2 Shape"); }
inline QString select() { return QStringLiteral("3 Select"); }
} // namespace ToolSections

struct ToolFactory {
    QString id;
    QString name;
    QString section;
    IconId icon{IconId::Brush};
    QKeySequence shortcut;
    int priority{0};
    std::function<Tool *()> create;
};

class ToolRegistry {
public:
    static ToolRegistry &instance();
    void add(ToolFactory f);
    const std::vector<ToolFactory> &factories() const { return factories_; }
    const ToolFactory *find(const QString &id) const;

private:
    std::vector<ToolFactory> factories_; // kept sorted: section, priority
};

// Registers the built-in tools. Idempotent.
void registerBuiltinTools();

} // namespace ink
