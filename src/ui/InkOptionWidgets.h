#pragma once
// Shared builders for tool options rows: one dense, flat vocabulary so every
// tool's option widget reads as part of the same design system.
#include <QString>

class QComboBox;
class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QSlider;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace ink {
namespace InkOptions {

// Small caps caption ("SIZE", "OPACITY", ...).
QLabel *caption(const QString &text, QWidget *parent);
// Right-hand value readout, fixed width so rows never jitter.
QLabel *value(const QString &text, QWidget *parent, int minWidth = 46);
QSlider *slider(int min, int max, int val, QWidget *parent, int width = 96);
QComboBox *combo(const QStringList &items, QWidget *parent, int width = 118);
// Checkable toggle: empty-box glyph when off, check glyph when on (both are
// code-drawn icons, so the state stays legible without asset files).
QToolButton *toggle(const QString &text, bool checked, QWidget *parent);
QToolButton *button(const QString &text, QWidget *parent);
// 1px vertical divider between option clusters.
QFrame *separator(QWidget *parent);
// caption + control, appended to a row layout.
void addField(QHBoxLayout *layout, const QString &caption, QWidget *control);
// ---- form grid (settings popups): every row shares one caption column, so
//     captions and controls line up in straight columns across the page. ----
// Caption column width shared by every formRow on the page.
inline constexpr int kFormCaptionWidth = 72;
// Section header: small-caps caption over a hairline rule, appended to a page.
void addSection(QVBoxLayout *page, const QString &text, QWidget *parent);
// One grid row: right-aligned caption + control (+ optional trailing widget,
// e.g. a value readout or a toggle). The grid must have 3 columns.
void formRow(QGridLayout *grid, int row, const QString &caption, QWidget *control,
             QWidget *trailing = nullptr);

} // namespace InkOptions
} // namespace ink
