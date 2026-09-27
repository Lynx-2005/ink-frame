#pragma once
// Parameter value text: ONE formatter and ONE parser, shared by every surface
// that shows or edits a keyframed value (the timeline bar, the transform
// panel's numeric fields). If the readout says 50%, a typed 50% must mean
// exactly that — two implementations would drift.
//
// Percent fields (opacity, scale) READ as percent but the model stores a
// fraction; angle fields carry a degree sign. The engine layer deliberately
// knows nothing about Qt, so this lives with the UI.
#include "core/InkParams.h"
#include <QString>
#include <vector>

namespace ink {

// Trim the trailing zeros a fixed-precision format leaves behind. Percent
// fields need a decimal to be USEABLE (opacity is a 0-100 range, and whole
// percents lose a 0.4% grade), but "100.0%" is just noise, and a 6-decimal
// angle/position value is unreadable in a narrow field. So: format wide enough
// to hold anything the range can express, then shave the padding.
inline QString trimNum(double v, int precision) {
    QString t = QString::number(v, 'f', precision);
    if (t.contains(QLatin1Char('.'))) {
        while (t.endsWith(QLatin1Char('0'))) t.chop(1);
        if (t.endsWith(QLatin1Char('.'))) t.chop(1);
    }
    return t;
}

inline QString formatParamValue(ParamId id, double v) {
    const ParamDesc &d = paramDesc(id);
    if (id == ParamId::Opacity || id == ParamId::ScaleX || id == ParamId::ScaleY)
        return trimNum(v * 100.0, 2) + QStringLiteral("%");
    if (d.angle) return trimNum(v, 3) + QString::fromUtf8("\xC2\xB0");
    return trimNum(v, 3);
}

// The inverse: "93.5%", "45deg" and "45" all parse, since artists paste
// whatever the readout showed.
inline double parseParamValue(ParamId id, const QString &text, bool *ok) {
    QString t = text;
    t.remove(QLatin1Char('%'));
    t.remove(QChar(0x00B0));
    t.remove(QStringLiteral("deg"), Qt::CaseInsensitive);
    const double raw = t.trimmed().toDouble(ok);
    if (ok && !*ok) return 0.0;
    if (id == ParamId::Opacity || id == ParamId::ScaleX || id == ParamId::ScaleY)
        return raw / 100.0; // the field reads percent, the model stores fraction
    return raw;
}

// The transform properties the UI offers, one dropdown entry each. The engine
// still stores X/Y as separate channels (the compositor needs both axes), but
// every SURFACE presents them as one group — one dropdown entry, one curve, one
// set of value fields — so the artist never picks between "Position X" and
// "Position Y" in a list.
// `graph` is the channel the easing curve edits when the group is selected
// (the first one of the group that actually has a segment); `field` lists the
// channels the value strip edits.
//
// Shear and Perspective are here too: both are keyed by the transform tool but
// had no dropdown entry, so their curves could not be shaped in the graph. The
// Perspective group exposes corner 1's channels; the other three corners are
// driven directly by the tool's on-canvas handles.
struct TransformGroup {
    const char *label;
    ParamId field[2];  // one or two channels (second == Count when scalar)
    ParamId graph[2];  // candidate channels for the curve, preference order
};

inline const std::vector<TransformGroup> &transformGroups() {
    static const std::vector<TransformGroup> kGroups = {
        {"Anchor", {ParamId::AnchorX, ParamId::AnchorY}, {ParamId::AnchorX, ParamId::AnchorY}},
        {"Position", {ParamId::PositionX, ParamId::PositionY}, {ParamId::PositionX, ParamId::PositionY}},
        {"Scale", {ParamId::ScaleX, ParamId::ScaleY}, {ParamId::ScaleX, ParamId::ScaleY}},
        {"Rotation", {ParamId::Rotation, ParamId::Count}, {ParamId::Rotation, ParamId::Count}},
        {"Shear", {ParamId::ShearX, ParamId::ShearY}, {ParamId::ShearX, ParamId::ShearY}},
        {"Perspective", {ParamId::Corner0X, ParamId::Corner0Y}, {ParamId::Corner0X, ParamId::Corner0Y}},
        {"Opacity", {ParamId::Opacity, ParamId::Count}, {ParamId::Opacity, ParamId::Count}},
    };
    return kGroups;
}

// The dropdown entry a channel belongs to, or -1 for the Perspective corners
// beyond the first (still keyable by the tool, just not standalone entries).
inline int transformGroupOf(ParamId id) {
    const auto &gs = transformGroups();
    for (size_t i = 0; i < gs.size(); ++i)
        if (gs[i].field[0] == id || gs[i].field[1] == id || gs[i].graph[0] == id ||
            gs[i].graph[1] == id)
            return int(i);
    return -1;
}

} // namespace ink
