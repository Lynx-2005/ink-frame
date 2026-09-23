#pragma once
// Dark-studio theme: single source of truth for colors, spacing,
// radii, row metrics and the layer hue palette. Applied as QSS in MainWindow;
// custom-painted widgets (timeline, layers) read the same QColors here
// so QSS + paint agree.
#include <QColor>
#include <QString>
#include <array>

namespace ink {

struct InkTheme {
    // --- surfaces ---
    static QColor bg() { return {26, 27, 31}; }
    static QColor chrome() { return {30, 31, 36}; }
    static QColor card() { return {38, 39, 44}; }
    static QColor lane() { return {28, 29, 33}; }
    static QColor border() { return {52, 53, 60}; }
    // --- text ---
    static QColor text() { return {232, 232, 234}; }
    static QColor dim() { return {150, 152, 160}; }
    static QColor faint() { return {110, 112, 120}; }
    // --- accents ---
    static QColor accent() { return {224, 138, 148}; }
    static QColor playhead() { return {224, 138, 148}; }
    static QColor hover() { return {52, 53, 60}; }
    static QColor warn() { return {217, 163, 74}; }        // locked rows
    static QColor accentMuted() { return {138, 106, 114}; } // secondary selection
    // --- tool chrome: flat, neutral, one functional accent (no candy) ---
    // Hover fill, checked block, glyph tones and the focus/accent line the
    // toolbox and tool options bar are built from.
    static QColor toolHover() { return {46, 48, 56}; }      // #2E3038
    static QColor toolChecked() { return {83, 114, 142}; }  // #53728E
    static QColor toolGlyph() { return {200, 200, 200}; }   // #C8C8C8
    static QColor toolGlyphOn() { return {235, 235, 235}; } // #EBEBEB
    static QColor focusRing() { return {138, 190, 236}; }   // #8ABEEC
    static QColor control() { return {38, 39, 44}; }        // #26272C (combo bg)
    // --- spacing scale ---
    static int sp(int i) { static const int s[5] = {4, 8, 12, 16, 24}; return s[i < 0 ? 0 : (i > 4 ? 4 : i)]; }
    static int radius() { return 10; }
    static int radiusSm() { return 6; }
    static int radiusCtl() { return 4; }
    static int radiusCell() { return 2; }
    // --- timeline metrics (shared with TimelineWidget paint) ---
    static int gutterW() { return 240; }
    static int rulerH() { return 30; }  // doubles as the gutter header row
    static int folderH() { return 26; }
    static int trackH() { return 30; }
    static int rowPad() { return 5; }
    // --- timeline surfaces (hairlines instead of fills) ---
    static QColor hairline() { return {44, 45, 52}; }
    static QColor laneAlt() { return {32, 33, 38}; }
    static QColor rowHover() { return {36, 37, 43}; }
    static QColor rowCurrent() { return {35, 36, 41}; }
    static QColor rowSelected() { return {32, 34, 40}; }
    static QColor tick() { return {58, 59, 66}; }
    static QColor outOfRange() { return {20, 21, 25}; }
    // --- paper cell (light neutral card, ink numbering) ---
    static QColor paper() { return {214, 210, 201}; }
    static QColor paperBlank() { return {194, 191, 182}; }
    static QColor paperEdge() { return {169, 165, 156}; }
    static QColor ink() { return {74, 72, 66}; }
    static QColor inkFaint() { return {110, 108, 102}; }
    // Layer hue palette (menus). Render through layerHue() so paint stays muted.
    static QColor clipColor(int i) {
        static const std::array<QColor, 8> k = {
            QColor{110, 140, 210}, QColor{140, 120, 190}, QColor{120, 175, 150},
            QColor{210, 140, 150}, QColor{150, 170, 220}, QColor{190, 160, 120},
            QColor{130, 160, 200}, QColor{170, 130, 180},
        };
        return k[i < 0 ? 0 : (i % 8)];
    }
    // Hue for painted chrome (folder spine, folder glyph): desaturated +
    // darkened so eight layers never turn the timeline into a color swatch.
    static QColor layerHue(int i) {
        QColor c = clipColor(i);
        const int g = qGray(c.rgb());
        const int k = 45; // % pulled toward grey
        c.setRed(c.red() + (g - c.red()) * k / 100);
        c.setGreen(c.green() + (g - c.green()) * k / 100);
        c.setBlue(c.blue() + (g - c.blue()) * k / 100);
        return c.darker(115);
    }
    // Hue for the thin rule along a cell's top edge: still muted, but light
    // enough to stay identifiable on the paper fill.
    static QColor layerHueBright(int i) {
        QColor c = clipColor(i);
        const int g = qGray(c.rgb());
        const int k = 25;
        c.setRed(c.red() + (g - c.red()) * k / 100);
        c.setGreen(c.green() + (g - c.green()) * k / 100);
        c.setBlue(c.blue() + (g - c.blue()) * k / 100);
        return c;
    }
    static QString styleSheet();
};

} // namespace ink
