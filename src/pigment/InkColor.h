#pragma once
// Pigment: color spaces + composite ops. Production-grade separation:
// - ColorSpace registry (sRGB8, LinearF32-concept via float conversion)
// - sRGB <-> linear conversions, premultiplication helpers
// - Composite ops operating on non-premultiplied RGBA8 with correct alpha
#include "core/InkTile.h"
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ink {

struct LinearRGBA { float r{0}, g{0}, b{0}, a{0}; };

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f/2.4f) - 0.055f;
}
LinearRGBA toLinear(PixelRGBA8 p);
PixelRGBA8 toSrgb(LinearRGBA l);

enum class ColorSpaceId { SRGB8, LinearSRGB };
class ColorSpace {
public:
    virtual ~ColorSpace() = default;
    virtual ColorSpaceId id() const = 0;
    virtual std::string name() const = 0;
};
class SRGB8Space : public ColorSpace {
public:
    ColorSpaceId id() const override { return ColorSpaceId::SRGB8; }
    std::string name() const override { return "sRGB (8-bit)"; }
};

class ColorSpaceRegistry {
public:
    static ColorSpaceRegistry &instance();
    const ColorSpace *get(ColorSpaceId id) const;
private:
    ColorSpaceRegistry();
    std::unordered_map<int, std::unique_ptr<ColorSpace>> spaces_;
};

// Composite ops (src-over family + blend). All take extraOpacity 0..1.
namespace composite {
PixelRGBA8 normal(PixelRGBA8 dst, PixelRGBA8 src, float o = 1.0f);
PixelRGBA8 multiply(PixelRGBA8 dst, PixelRGBA8 src, float o = 1.0f);
PixelRGBA8 screen(PixelRGBA8 dst, PixelRGBA8 src, float o = 1.0f);
PixelRGBA8 erase(PixelRGBA8 dst, PixelRGBA8 src, float o = 1.0f);
} // namespace composite

} // namespace ink
