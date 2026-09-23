#pragma once
// Hand-painted vector icon factory. Every glyph is drawn with QPainter
// paths — no emoji, no asset files, always crisp at any size/DPR and
// theme-aware (normal / disabled / active-accent variants).
// Replaces all emoji glyphs across ToolRail, LayerDock, Timeline, TopBar.
#include <QColor>
#include <QIcon>

namespace ink {

enum class IconId {
    Brush, Eraser, Shape, Picker, Text, Note, Select,
    Eye, EyeOff, Lock, Unlock, Folder, FolderOpen,
    Play, Pause, PrevKey, NextKey, SkipStart, SkipEnd,
    Plus, Minus, Close, Check,
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
