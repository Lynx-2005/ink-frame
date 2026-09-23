#include "pigment/InkColor.h"
#include <algorithm>
#include <cmath>

namespace ink {

LinearRGBA toLinear(PixelRGBA8 p) {
    return {srgbToLinear(p.r/255.f), srgbToLinear(p.g/255.f), srgbToLinear(p.b/255.f), p.a/255.f};
}
PixelRGBA8 toSrgb(LinearRGBA l) {
    auto c = [](float v){ return static_cast<uint8_t>(std::clamp(linearToSrgb(std::clamp(v,0.f,1.f)),0.f,1.f)*255.f); };
    return {c(l.r), c(l.g), c(l.b), static_cast<uint8_t>(std::clamp(l.a,0.f,1.f)*255.f)};
}

ColorSpaceRegistry &ColorSpaceRegistry::instance() {
    static ColorSpaceRegistry r;
    return r;
}
ColorSpaceRegistry::ColorSpaceRegistry() {
    spaces_[0] = std::make_unique<SRGB8Space>();
}
const ColorSpace *ColorSpaceRegistry::get(ColorSpaceId id) const {
    auto it = spaces_.find((int)id);
    return it == spaces_.end() ? nullptr : it->second.get();
}

namespace composite {
static float blend(float sa, float da, float o) { return sa + da*(1-sa*o); }
PixelRGBA8 normal(PixelRGBA8 dst, PixelRGBA8 src, float o) {
    float sa = (src.a/255.f)*o, da = dst.a/255.f;
    float outA = sa + da*(1-sa);
    if (outA <= 1e-6f) return {0,0,0,0};
    auto ch = [&](uint8_t s, uint8_t d){
        return static_cast<uint8_t>(std::clamp((s*sa + d*da*(1-sa))/outA, 0.f, 255.f));
    };
    return {ch(src.r,dst.r), ch(src.g,dst.g), ch(src.b,dst.b),
            static_cast<uint8_t>(std::clamp(outA*255.f,0.f,255.f))};
}
PixelRGBA8 multiply(PixelRGBA8 dst, PixelRGBA8 src, float o) {
    PixelRGBA8 m{static_cast<uint8_t>(dst.r*src.r/255), static_cast<uint8_t>(dst.g*src.g/255),
                 static_cast<uint8_t>(dst.b*src.b/255), src.a};
    return normal(dst, m, o);
}
PixelRGBA8 screen(PixelRGBA8 dst, PixelRGBA8 src, float o) {
    auto sc = [](uint8_t d, uint8_t s){ return static_cast<uint8_t>(255 - (255-d)*(255-s)/255); };
    PixelRGBA8 m{sc(dst.r,src.r), sc(dst.g,src.g), sc(dst.b,src.b), src.a};
    return normal(dst, m, o);
}
PixelRGBA8 erase(PixelRGBA8 dst, PixelRGBA8 src, float o) {
    float cut = (src.a/255.f)*o;
    dst.a = static_cast<uint8_t>(dst.a*(1-cut));
    (void)blend;
    return dst;
}
} // namespace composite

} // namespace ink
