#pragma once
// Fill bucket: flood fill sampled from the merged image (or the current
// layer), clipped by the active selection.
#include "ui/tools/InkTool.h"

namespace ink {

class ToolContext;

class FillTool : public Tool {
public:
    QString id() const override { return QStringLiteral("fill"); }
    void activate(ToolContext &ctx) override;
    void beginPrimary(const ToolEvent &e) override;
    QCursor cursor() const override { return QCursor(Qt::CrossCursor); }
    QWidget *createOptionsWidget(QWidget *parent) override;
    QString statusHint() const override {
        return QStringLiteral("Click an area to fill it · Shift+click fills every similar pixel");
    }

private:
    bool sampleMerged_{true};
    int tolerance_{0}; // 0..255
    bool contiguous_{true};
};

} // namespace ink
