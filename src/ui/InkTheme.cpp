#include "ui/InkTheme.h"

namespace ink {

QString InkTheme::styleSheet() {
    return QStringLiteral(
        // base
        "QMainWindow, QWidget { background: #1A1B1F; color: #E8E8EA; font-size: 13px; }"
        "QToolTip { background: #34353C; color: #E8E8EA; border: 1px solid #45464E; padding: 4px 8px; }"
        // docks become invisible chrome: the Card widget inside carries style
        "QDockWidget { border: none; titlebar-close-icon: none; titlebar-normal-icon: none; font-weight: 600; }"
        "QDockWidget::title { background: #1E1F24; padding: 2px 8px; margin: 0px; border: none; color: #9698A0; font-size: 11px; font-weight: 600; }"
        "QDockWidget::close-button, QDockWidget::float-button { border: none; background: transparent; icon-size: 0px; width: 0px; height: 0px; }"
        // cards
        "QWidget#Card { background: #26272C; border: 1px solid #34353C; border-radius: 10px; }"
        "QWidget#TopBar { background: #1E1F24; border-bottom: 1px solid #34353C; }"
        "QWidget#ToolRail { background: #1E1F24; border-right: 1px solid #34353C; }"
        "QWidget#TransportBar { background: transparent; }"
        // buttons
        "QToolButton { background: transparent; border: 1px solid transparent; border-radius: 8px; padding: 6px; color: #E8E8EA; icon-size: 20px; }"
        "QToolButton:hover { background: #34353C; }"
        "QToolButton:checked { background: #3A2B2E; border: 1px solid #E08A94; }"
        "QToolButton:disabled { color: #6E7078; }"
        "QToolButton#RailBtn { min-width: 42px; min-height: 42px; icon-size: 22px; }"
        "QToolButton#Well { border-radius: 18px; min-width: 34px; min-height: 34px; padding: 1px; }"
        "QPushButton { background: #34353C; border: 1px solid #45464E; border-radius: 8px; padding: 5px 10px; }"
        "QPushButton:hover { background: #45464E; }"
        "QPushButton:disabled { color: #6E7078; }"
        // inputs
        "QComboBox, QSpinBox, QSlider { background: #34353C; border: 1px solid #45464E; border-radius: 8px; padding: 4px 8px; }"
        "QComboBox QAbstractItemView { background: #26272C; border: 1px solid #45464E; selection-background-color: #3A3B42; }"
        "QLineEdit { background: #202127; border: 1px solid #E08A94; border-radius: 6px; padding: 3px 6px; selection-background-color: #E08A94; }"
        // item views
        "QTreeWidget, QListWidget, QTableView { background: #26272C; border: none; alternate-background-color: #202127; show-decoration-selected: 1; outline: 0; }"
        "QTreeWidget::item, QListWidget::item { padding: 4px; border-radius: 6px; }"
        "QTreeWidget::item:selected, QListWidget::item:selected { background: #3A4054; }"
        "QTreeWidget::item:hover, QListWidget::item:hover { background: #34353C; }"
        "QTreeWidget::branch { background: transparent; }"
        "QHeaderView::section { background: #202127; border: none; padding: 4px; color: #9698A0; }"
        // scrollbars: slim overlay look, visible handle
        "QScrollBar:vertical { background: #202127; width: 10px; margin: 2px; border-radius: 4px; }"
        "QScrollBar:horizontal { background: #202127; height: 10px; margin: 2px; border-radius: 4px; }"
        "QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: #4E5058; border-radius: 4px; min-height: 28px; min-width: 28px; }"
        "QScrollBar::handle:hover { background: #62646C; }"
        "QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
        // menus
        "QMenuBar { background: #1E1F24; }"
        "QMenu { background: #2A2B31; border: 1px solid #45464E; border-radius: 8px; padding: 4px; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 6px; }"
        "QMenu::item:selected { background: #3A4054; }"
        "QMenu::separator { height: 1px; background: #34353C; margin: 4px 8px; }"
        // labels
        "QLabel#Logo { font-size: 22px; font-style: italic; font-weight: 300; color: #E8E8EA; }"
        "QLabel#Timecode { font-family: monospace; font-size: 14px; color: #E8E8EA; }"
        "QLabel#Hint { color: #9698A0; font-size: 12px; }"
        "QLabel#Section { color: #9698A0; font-size: 11px; font-weight: 600; }"
        // sliders in tool options
        "QSlider::groove:horizontal { height: 4px; background: #34353C; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -6px 0; border-radius: 7px; background: #E08A94; }"
        // --- timeline toolbar: dense, hairline separated, 4px radii ---
        "QWidget#TimelineToolbar { background: #1E1F24; border-bottom: 1px solid #2C2D34; }"
        "QWidget#TimelineToolbar QToolButton { padding: 3px; border: 1px solid transparent; border-radius: 4px; icon-size: 16px; min-width: 24px; min-height: 24px; }"
        "QWidget#TimelineToolbar QToolButton:hover { background: #2E3038; border-color: #3A3B42; }"
        "QWidget#TimelineToolbar QToolButton:checked { background: rgba(224, 138, 148, 0.16); border-color: rgba(224, 138, 148, 0.55); }"
        "QWidget#TimelineToolbar QToolButton:disabled { color: #5A5C64; }"
        "QWidget#TimelineToolbar QToolButton#TransportPlay { min-width: 28px; min-height: 28px; icon-size: 18px; }"
        "QWidget#TimelineToolbar QLabel { color: #9698A0; font-size: 11px; }"
        "QWidget#TimelineToolbar QLabel#Timecode { font-family: monospace; font-size: 12px; color: #E8E8EA; }"
        "QWidget#TimelineToolbar QLabel#Capt { color: #7C7E88; font-size: 9px; font-weight: 700; }"
        "QWidget#TimelineToolbar QLabel#Dim { color: #6E7078; font-size: 11px; }"
        "QWidget#TimelineToolbar QSpinBox, QWidget#TimelineToolbar QComboBox { background: #24252B; border: 1px solid #2C2D34; border-radius: 4px; padding: 1px 4px; min-height: 20px; max-height: 22px; font-size: 11px; }"
        "QWidget#TimelineToolbar QSpinBox:focus, QWidget#TimelineToolbar QComboBox:focus { border-color: #E08A94; }"
        "QWidget#TimelineToolbar QComboBox QAbstractItemView { background: #26272C; border: 1px solid #45464E; }"
        "QWidget#TimelineToolbar QSlider { background: transparent; border: none; padding: 0; min-height: 22px; }"
        "QWidget#TimelineToolbar QSlider::groove:horizontal { height: 3px; background: #2C2D34; border-radius: 2px; }"
        "QWidget#TimelineToolbar QSlider::handle:horizontal { width: 10px; margin: -5px 0; border-radius: 5px; background: #8A8C96; }"
        "QWidget#TimelineToolbar QSlider::handle:horizontal:hover { background: #E08A94; }"
        "QFrame#Sep { background: #2C2D34; border: none; min-width: 1px; max-width: 1px; }"
        "QLabel#TimelineStatus { color: #6E7078; font-size: 10px; }"
    );
}

} // namespace ink
