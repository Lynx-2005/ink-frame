#pragma once
// ViewConverter: single source of truth for document<->widget mapping.
// Clean-room analogue of Krita's KisCoordinatesConverter concept:
// ALL derived transforms are rebuilt in recalculate() from view state;
// paint code and event code share this object, never parallel math.
// Logical pixels in/out; devicePixelRatio only affects physicalScale()
// (used by the future GL texture path, never by hit-testing).
// Pure std C++ — headless unit-testable, no Qt dependency.
#include <algorithm>
#include <cmath>

namespace ink {

struct Vec2 {
    double x{0}, y{0};
    Vec2 operator+(const Vec2 &o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2 &o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
};

// 2D affine: [ a c tx ]   (maps column vector)
//            [ b d ty ]
struct Affine2D {
    double a{1}, b{0}, c{0}, d{1}, tx{0}, ty{0};
    Vec2 map(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    Affine2D inverted() const {
        double det = a * d - b * c;
        // Degenerate (zoom 0) — cannot happen via public API; guard anyway.
        if (std::abs(det) < 1e-12) return {};
        double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }
};

class ViewConverter {
public:
    ViewConverter() = default;

    // --- state (all logical pixels / degrees) ---
    void setDocSize(double w, double h);
    void setWidgetSize(double w, double h);
    void setDevicePixelRatio(double dpr) { dpr_ = dpr > 0 ? dpr : 1.0; }

    double zoom() const { return zoom_; }
    double rotationDeg() const { return rotationDeg_; }
    Vec2 pan() const { return pan_; }

    // --- transforms (rebuilt by recalculate) ---
    const Affine2D &docToWidget() const { return docToWidget_; }
    const Affine2D &widgetToDoc() const { return widgetToDoc_; }
    Vec2 mapToWidget(Vec2 doc) const { return docToWidget_.map(doc); }
    Vec2 mapToDoc(Vec2 widget) const { return widgetToDoc_.map(widget); }

    // Fit whole document into widget with margin (0..1 of available space).
    void fit(double margin = 0.95);
    // Zoom keeping the document point under widget point w fixed (still-point).
    void zoomAt(Vec2 w, double factor, double minZoom = 0.02, double maxZoom = 64.0);
    void zoomAtCenter(double factor) {
        zoomAt({widgetW_ * 0.5, widgetH_ * 0.5}, factor);
    }
    void panBy(Vec2 d) { pan_ = pan_ + d; recalculate(); }
    void setRotation(double deg);

    // Document rect currently visible (for culling), in doc pixels.
    void visibleDocRect(double &x, double &y, double &w, double &h) const;
    // Scale for physical (device-pixel) work such as GL uploads.
    double physicalScale() const { return zoom_ * dpr_; }

private:
    void recalculate();

    double docW_{1}, docH_{1};
    double widgetW_{1}, widgetH_{1};
    double dpr_{1};
    double zoom_{1};
    double rotationDeg_{0};
    Vec2 pan_{0, 0}; // widget-space offset of doc center from widget center
    Affine2D docToWidget_, widgetToDoc_;
};

inline void ViewConverter::setDocSize(double w, double h) {
    if (w > 0) docW_ = w;
    if (h > 0) docH_ = h;
    recalculate();
}

inline void ViewConverter::setWidgetSize(double w, double h) {
    if (w > 0) widgetW_ = w;
    if (h > 0) widgetH_ = h;
    recalculate();
}

inline void ViewConverter::setRotation(double deg) {
    rotationDeg_ = std::fmod(deg, 360.0);
    recalculate();
}

inline void ViewConverter::recalculate() {
    // docToWidget(p) = R * S * (p - docCenter) + widgetCenter + pan
    const double rad = rotationDeg_ * M_PI / 180.0;
    const double co = std::cos(rad) * zoom_;
    const double si = std::sin(rad) * zoom_;
    // R*S = [ co -si ; si co ]
    const double cx = docW_ * 0.5, cy = docH_ * 0.5;
    const double wx = widgetW_ * 0.5 + pan_.x, wy = widgetH_ * 0.5 + pan_.y;
    // t = widgetCenter + pan - R*S*docCenter
    docToWidget_ = {co, si, -si, co, wx - (co * cx - si * cy), wy - (si * cx + co * cy)};
    widgetToDoc_ = docToWidget_.inverted();
}

inline void ViewConverter::fit(double margin) {
    margin = std::clamp(margin, 0.05, 1.0);
    zoom_ = std::min(widgetW_ / docW_, widgetH_ / docH_) * margin;
    zoom_ = std::clamp(zoom_, 0.02, 64.0);
    pan_ = {0, 0};
    recalculate();
}

inline void ViewConverter::zoomAt(Vec2 w, double factor, double minZoom, double maxZoom) {
    if (!(factor > 0)) return;
    const Vec2 doc = mapToDoc(w); // still point: must stay under cursor
    zoom_ = std::clamp(zoom_ * factor, minZoom, maxZoom);
    recalculate();
    // Re-anchor: pan' = w - (R*S*(doc - docCenter) + widgetCenter)
    const Vec2 anchored = mapToDoc(w);
    // Correct residual by shifting pan in widget space (rotation-safe):
    // move pan so that mapToDoc(w) == doc.
    const Vec2 err = {anchored.x - doc.x, anchored.y - doc.y};
    // Shift pan by the forward-mapped error so mapToDoc(w) == doc again:
    // pan' = pan + R'*S'*(anchored - doc)  (translation commutes through).
    const double rad = rotationDeg_ * M_PI / 180.0;
    const double co = std::cos(rad) * zoom_;
    const double si = std::sin(rad) * zoom_;
    pan_.x += co * err.x - si * err.y;
    pan_.y += si * err.x + co * err.y;
    recalculate();
}

inline void ViewConverter::visibleDocRect(double &x, double &y, double &w, double &h) const {
    Vec2 tl = mapToDoc({0, 0});
    Vec2 tr = mapToDoc({widgetW_, 0});
    Vec2 bl = mapToDoc({0, widgetH_});
    Vec2 br = mapToDoc({widgetW_, widgetH_});
    double x0 = std::min(std::min(tl.x, tr.x), std::min(bl.x, br.x));
    double x1 = std::max(std::max(tl.x, tr.x), std::max(bl.x, br.x));
    double y0 = std::min(std::min(tl.y, tr.y), std::min(bl.y, br.y));
    double y1 = std::max(std::max(tl.y, tr.y), std::max(bl.y, br.y));
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;
}

} // namespace ink
