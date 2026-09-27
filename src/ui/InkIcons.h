#pragma once
// Hand-painted vector icon factory. Every glyph is drawn with QPainter
// paths — no emoji, no asset files, always crisp at any size/DPR and
// theme-aware (normal / disabled / active variants).
// Tool glyphs are drawn on a 24x24 design grid with 1.6-unit strokes so the
// whole set reads as one professional family (see grid24 in the .cpp).
#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

namespace ink {

enum class IconId {
    // tools, in toolbox order
    Brush, Eraser, Fill, Picker, Move, Line, Rectangle, Ellipse,
    SelectRect, SelectEllipse, Lasso,
    Transform, Warp, Liquify, Camera,
    Swap, ResetColor,
    // layer / timeline chrome
    Eye, EyeOff, Lock, Unlock, Folder, FolderOpen,
    Play, Pause, PrevKey, NextKey, SkipStart, SkipEnd,
    Plus, Minus, Close, Check, CheckBox,
    ChevronLeft, ChevronRight,
    Onion, Loop, Once, PingPong,
    Undo, Redo, Menu, More, Fit, Key,
    // cells (paper sheets)
    Cell, CellPlus, CellDuplicate, CellRemove,
    // layers / groups
    LayerPlus, GroupPlus, GroupMerge, Ungroup, FollowHead,
    // keyframes / adjustment layers
    Adjustment,      // "A" plate: the layer that carries the animation
    KeyPlus, KeyRemove, KeyPrev, KeyNext,
    Stopwatch,       // per-parameter animation toggle
    Graph,           // easing graph toggle
    Link, LinkBroken,
    PresetEase, PresetHold, PresetLinear,
};

class InkIcons {
public:
    // size = pixel size of square pixmap. active=true tints with accent.
    static QIcon get(IconId id, int size = 20, bool active = false);
    static QIcon get(IconId id, const QColor &color, int size = 20);
    // Small chevron centred in `cell`, pointing up or down. Fields paint their
    // own drop-down/stepper glyphs with it: once a sub-control is styled, Qt
    // skips its built-in arrow and this project ships no image assets.
    static void drawChevron(QPainter &p, const QRectF &cell, bool up, const QColor &color,
                            double size = 8.0);
};

} // namespace ink
