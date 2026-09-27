#pragma once
// Camera tool: Clip-Studio-style 2D camera control. Drag to pan the shot,
// Ctrl+drag vertically to dolly (uniform zoom), Shift+drag horizontally to
// roll. Every gesture writes camera keyframes (one undo step per drag) when
// Auto-key is armed, else it edits the camera's static pose. Arrows nudge.
#include "ui/tools/InkTool.h"

namespace ink {

class CameraLayer;

class CameraTool : public Tool {
public:
    QString id() const override { return QStringLiteral("camera"); }
    void activate(ToolContext &ctx) override;
    void deactivate() override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    void endPrimary(const ToolEvent &e) override;
    void cancelPrimary() override;
    bool nudge(int dx, int dy, Qt::KeyboardModifiers mods) override;
    bool isEditing() const override { return dragging_; }
    QCursor cursor() const override { return QCursor(Qt::OpenHandCursor); }
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override;

private:
    CameraLayer *resolveTarget() const;
    ToolContext *ctx_{nullptr};
    CameraLayer *cam_{nullptr};
    int frame_{-1};
    bool dragging_{false};
    Vec2 start_{};
    Vec2 last_{};
};

} // namespace ink
