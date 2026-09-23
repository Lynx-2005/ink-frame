#include "resources/InkResources.h"

namespace ink {

const BrushPreset *ResourceDb::find(const std::string &id) const {
    for (auto &p : presets_) if (p.id == id) return &p;
    return nullptr;
}

ResourceDb ResourceDb::defaults() {
    ResourceDb db;
    BrushPreset pen;
    pen.id = "ink_pen"; pen.name = "Ink Pen";
    pen.tip = BrushTipShape{8.0f, 0.9f, 1.0f, 0.06f};
    pen.dynamics.sizePressure = SensorCurve{1.0f, true};
    pen.dynamics.smoothing = SmoothingConfig{SmoothingMode::Weighted, 0.30f, true};
    pen.color = PixelRGBA8{20, 20, 20, 255};
    pen.engine = "pixel_brush";
    db.addPreset(pen);

    BrushPreset soft;
    soft.id = "soft_round"; soft.name = "Soft Round";
    soft.tip = BrushTipShape{64.0f, 0.15f, 0.8f, 0.08f};
    soft.dynamics.sizePressure = SensorCurve{0.6f, true};
    soft.dynamics.smoothing = SmoothingConfig{SmoothingMode::Weighted, 0.45f, true};
    soft.color = PixelRGBA8{30, 30, 30, 255};
    soft.engine = "pixel_brush";
    db.addPreset(soft);

    BrushPreset marker;
    marker.id = "marker"; marker.name = "Marker";
    marker.tip = BrushTipShape{24.0f, 1.0f, 1.0f, 0.05f};
    marker.dynamics.sizePressure = SensorCurve{0.3f, true};
    marker.dynamics.smoothing = SmoothingConfig{SmoothingMode::Weighted, 0.15f, true};
    marker.color = PixelRGBA8{20, 40, 200, 255};
    marker.engine = "pixel_brush";
    db.addPreset(marker);

    BrushPreset eraser;
    eraser.id = "eraser"; eraser.name = "Eraser";
    eraser.tip = BrushTipShape{32.0f, 0.8f, 1.0f, 0.08f};
    eraser.dynamics.sizePressure = SensorCurve{1.0f, true};
    eraser.color = PixelRGBA8{0, 0, 0, 255};
    eraser.engine = "eraser";
    db.addPreset(eraser);
    return db;
}

} // namespace ink
