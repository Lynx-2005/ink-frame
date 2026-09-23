#pragma once
// Color picker: samples the flattened frame projection (what the user sees),
// click for the foreground, Ctrl+click for the background.
#include "ui/tools/InkTool.h"

namespace ink {

class ToolContext;

class ColorPickerTool : public Tool {
public:
    QString id() const override { return QStringLiteral("picker"); }
    void activate(ToolContext &ctx) override;
    void beginPrimary(const ToolEvent &e) override;
    void continuePrimary(const ToolEvent &e) override;
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override {
        return QStringLiteral("Click to sample a color · Ctrl+click samples the background");
    }

private:
    void sample(const ToolEvent &e);
};

} // namespace ink
