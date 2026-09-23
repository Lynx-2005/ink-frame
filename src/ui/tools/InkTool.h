#pragma once
// Tool framework core: one class per tool, registry-driven.
// Clean-room reading of Krita's tool architecture (KoToolBase/KisTool concept):
// a tool has a lifecycle, a primary-action event stream, an optional overlay
// painter and its own options widget. No Krita code involved.
#include "core/InkViewConverter.h" // Vec2
#include <QCursor>
#include <QObject>
#include <QString>

class QPainter;
class QWidget;

namespace ink {

class ToolContext;

// One pointer event, already mapped into document space by the canvas.
struct ToolEvent {
    Vec2 docPos;
    float pressure{1.0f};
    float tiltX{0.0f};
    float tiltY{0.0f};
    double timeSec{0.0};
    Qt::KeyboardModifiers mods{};
    bool tablet{false};
};

// What the canvas exposes to tools without tools including the canvas header.
struct ToolHost {
    virtual ~ToolHost() = default;
    virtual void repaint() = 0;  // overlay-only redraw
    virtual void rebuild() = 0;  // full projection rebuild
};

// Overlay draw info; the painter is already in document space.
struct OverlayInfo {
    double zoom{1.0};
    bool hover{false};
    Vec2 hoverPos;
};

class Tool : public QObject {
public:
    using QObject::QObject;
    ~Tool() override = default;

    virtual QString id() const = 0;

    // lifecycle
    virtual void activate(ToolContext &ctx) { ctx_ = &ctx; }
    virtual void deactivate() {}

    // primary action (mouse/stylus press-drag-release)
    virtual void beginPrimary(const ToolEvent &) {}
    virtual void continuePrimary(const ToolEvent &) {}
    virtual void endPrimary(const ToolEvent &) {}
    virtual void cancelPrimary() {}
    // Arrow-key nudging; return true when the tool consumed the key.
    virtual bool nudge(int dx, int dy, Qt::KeyboardModifiers mods) {
        Q_UNUSED(dx);
        Q_UNUSED(dy);
        Q_UNUSED(mods);
        return false;
    }
    // Mirror the current region (selection contents, else the layer).
    virtual void flipSelection(bool horizontal) { Q_UNUSED(horizontal); }

    // true while this tool has uncommitted edits on a device: the canvas
    // must bypass its frame cache so dabs show up live.
    virtual bool isEditing() const { return false; }

    virtual QCursor cursor() const { return QCursor(Qt::ArrowCursor); }
    virtual void paintOverlay(QPainter &, const OverlayInfo &) const {}
    virtual QWidget *createOptionsWidget(QWidget *parent) {
        Q_UNUSED(parent);
        return nullptr;
    }
    virtual QString statusHint() const { return {}; }

protected:
    ToolContext *ctx_{nullptr};
};

} // namespace ink
