#pragma once
// Hand-painted vector icon factory. Every glyph is drawn with QPainter
// paths — no emoji, no asset files, always crisp at any size/DPR and
// theme-aware (normal / disabled / active variants).
// Tool glyphs are drawn on a 24x24 design grid with 1.6-unit strokes so the
// whole set reads as one professional family (see grid24 in the .cpp).
#include <QColor>
#include <QIcon>

namespace ink {

enum class IconId {
    // tools, in toolbox order
    Brush, Eraser, Fill, Picker, Line, Rectangle, Ellipse,
    SelectRect, SelectEllipse, Lasso,
    Swap, ResetColor,
    // layer / timeline chrome
    Eye, EyeOff, Lock, Unlock, Folder, FolderOpen,
    Play, Pause, PrevKey, NextKey, SkipStart, SkipEnd,
    Plus, Minus, Close, Check, CheckBox,
    ChevronLeft, ChevronRight,
    Onion, Loop, Once, PingPong,
    Undo, Redo, Menu, Fit, Key,
    // cells (paper sheets)
    Cell, CellPlus, CellDuplicate, CellRemove,
    // layers / groups
    LayerPlus, GroupPlus, GroupMerge, Ungroup, FollowHead,
};

class InkIcons {
public:
    // size = pixel size of square pixmap. active=true tints with accent.
    static QIcon get(IconId id, int size = 20, bool active = false);
    static QIcon get(IconId id, const QColor &color, int size = 20);
};

} // namespace ink
