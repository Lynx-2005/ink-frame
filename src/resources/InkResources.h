#pragma once
// Brush presets + resource DB (file-backed v1; SQLite via Qt in UI phase).
// Production: versioned preset schema, tags, default set shipped with app.
#include <string>
#include <vector>
#include "brush/InkBrushTip.h"
#include "brush/InkSensors.h"
#include "core/InkTile.h"

namespace ink {

struct BrushPreset {
    std::string id;
    std::string name;
    BrushTipShape tip;
    BrushDynamics dynamics;
    PixelRGBA8 color{20,20,20,255};
    std::string engine{"pixel_brush"}; // or "eraser"
    int version{1};
};

class ResourceDb {
public:
    void addPreset(BrushPreset p) { presets_.push_back(std::move(p)); }
    const std::vector<BrushPreset> &presets() const { return presets_; }
    const BrushPreset *find(const std::string &id) const;
    static ResourceDb defaults(); // ships: ink pen, soft brush, marker, eraser
private:
    std::vector<BrushPreset> presets_;
};

} // namespace ink
