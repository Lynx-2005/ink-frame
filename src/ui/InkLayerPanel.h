#pragma once
// LayerPanel: the right-side "Layer" panel.
//
// It edits the layer stack of the CELL exposed at the playhead on the current
// track — a per-cell stack, unique to that cell (InkCell.h). This is a
// different thing from the timeline's tracks: a track is a row in the timeline
// that holds cells; a layer here lives INSIDE one cell.
//
// The visual language mirrors the timeline gutter (same InkTheme metrics,
// colors and row states) with one deliberate difference: the panel shows the
// FRONT-most layer at the TOP, the way every layer palette reads. The gutter
// shows the back-most first. Do not "fix" either one to match the other.
#include "core/InkCell.h"
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QWidget>
#include <cstdint>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QScrollArea;
class QSlider;
class QToolButton;

namespace ink {

class Document;

// One painted row of the list (built by the panel from the current cell).
struct LayerRow {
    CellLayer *layer{nullptr};
    uint64_t id{0};
    uint64_t parentId{0}; // 0 = top level (stack order)
    int index{0};         // position among the parent's children
    int childCount{0};    // groups: children to append into on drop
    int depth{0};
    bool isGroup{false};
    bool expanded{true};
    // Selection checkbox (the control used to group layers): a paint layer is
    // ticked when it is selected; a group is ticked when every paint layer
    // below it is (and its box is disabled when it holds none).
    bool checked{false};
    bool checkEnabled{true};
    // Inherited from a group above: the layer's own flags say "on", but the
    // group hides or locks it. The row is drawn dimmed so the two agree.
    bool hiddenByGroup{false};
    bool lockedByGroup{false};
    int y{0};
    int h{0};
};

// The scrollable list. Painting + hit-testing + drag/drop live here; every
// model change is emitted as a signal and performed by the panel.
class LayerListView : public QWidget {
    Q_OBJECT
public:
    explicit LayerListView(QWidget *parent = nullptr);

    void setRows(const std::vector<LayerRow> &rows, uint64_t currentId, const QSet<uint64_t> &sel);
    QRect rowRect(uint64_t id) const;
    QRect nameRect(uint64_t id) const;
    // Selection checkbox rect for a row (paint + hit-testing share it).
    QRect checkBoxRect(const LayerRow &r) const;
    uint64_t idAt(const QPoint &pos) const;
    void setEmptyText(const QString &title, const QString &sub);
    // Inline rename: the editor parks over the row's name column.
    void beginRename(uint64_t id, const QString &text);
    void cancelRename();
    bool renaming() const { return editor_ != nullptr; }
    void invalidateThumbs() { thumbs_.clear(); update(); }
    void invalidateThumb(uint64_t id) { thumbs_.remove(id); update(); }
    QPixmap thumbFor(const CellLayer *layer);

signals:
    void selectRequested(uint64_t id, Qt::KeyboardModifiers mods);
    void checkToggled(uint64_t id);
    void visibilityToggled(uint64_t id);
    void lockToggled(uint64_t id);
    void expandToggled(uint64_t id);
    void renameCommitted(uint64_t id, const QString &name);
    void menuRequested(uint64_t id, const QPoint &globalPos);
    void moveRequested(uint64_t id, uint64_t parentId, int index);
    void deleteRequested(uint64_t id);
    void groupRequested();
    void ungroupRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    QSize sizeHint() const override;

private:
    struct Columns {
        int expander{0}, check{0}, chip{0}, name{0}, eye{0}, lock{0};
    };
    Columns columns() const;
    // What sits under a point: 0 = row body, 1 = expander, 2 = checkbox,
    // 3 = eye, 4 = lock.
    int partAt(const QPoint &pos, uint64_t &id) const;
    void drawCheckBox(QPainter &p, const QRect &box, bool checked, bool enabled) const;
    void commitRename();
    void drawChevron(QPainter &p, const QRectF &r, bool open, const QColor &c) const;

    std::vector<LayerRow> rows_;
    uint64_t currentId_{0};
    QSet<uint64_t> sel_;
    uint64_t hoverId_{0};
    int hoverPart_{0};
    QString emptyTitle_;
    QString emptySub_;
    // drag & drop
    uint64_t pressId_{0};
    QPoint pressPos_;
    bool dragging_{false};
    bool dropValid_{false};
    uint64_t dropParent_{0};
    int dropIndex_{-1};
    QRect dropIndicator_;   // boundary line, in widget coordinates
    QRect dropIntoRect_;    // group highlight for a drop-into-group
    QLineEdit *editor_{nullptr};
    uint64_t editId_{0};
    bool editCancelled_{false};
    QHash<uint64_t, QPixmap> thumbs_;
};

// The panel: header (title + context line + buttons), the list, and the
// opacity/blend inspector for the selected layer.
class LayerPanel : public QWidget {
    Q_OBJECT
public:
    // Build (but do not show) the menu for a row. The ticked-set items live
    // here, so this is also how a test reads what a right-click offers.
    QMenu *buildContextMenu(uint64_t id);
    explicit LayerPanel(Document *doc, QWidget *parent = nullptr);

    void refresh();          // rebuild rows from the model
    void invalidateThumbs(); // pixels changed: drop cached chips
    void invalidateThumb(uint64_t id); // one layer's chip (stroke-commit path)
    QWidget *listWidget() const; // for tests

signals:
    // The model changed (structure or properties): repaint canvas + timeline.
    void cellLayersChanged();

private:
    Cell *cell() const;
    std::vector<CellLayer*> selectedLayers() const;
    CellLayer *singleSelection() const;
    void buildRows();
    void syncInspector();
    void syncButtons();
    void onSelect(uint64_t id, Qt::KeyboardModifiers mods);
    void onCheck(uint64_t id); // tick/untick a row (groups tick their contents)
    void onMove(uint64_t id, uint64_t parentId, int index);
    void showAddMenu();
    void showContextMenu(uint64_t id, const QPoint &globalPos);
    void beginRename(uint64_t id);
    // New layers/groups always land at the top of the stack, top level, so they
    // stay independent of whatever is selected.
    void opAddLayer();
    void opAddGroup();
    void opDuplicate(uint64_t id);
    void opDelete(uint64_t id);
    void opDeleteSelected(); // every ticked layer, one undo step each
    void opGroup();
    void opUngroup();
    void opMove(uint64_t id, int delta);
    void opMoveToGroup(uint64_t id, uint64_t groupId); // 0 = top level
    void afterOp(); // emit + refresh

    Document *doc_;
    QLabel *title_{nullptr};
    QLabel *context_{nullptr};
    QToolButton *addBtn_{nullptr};
    QToolButton *groupBtn_{nullptr};
    QToolButton *ungroupBtn_{nullptr};
    LayerListView *list_{nullptr};
    QScrollArea *scroll_{nullptr};
    QSlider *opacity_{nullptr};
    QLabel *opacityValue_{nullptr};
    QComboBox *blend_{nullptr};
    std::vector<LayerRow> rows_;
    QSet<uint64_t> sel_;
    uint64_t anchor_{0};
    QSet<uint64_t> collapsed_; // group ids the user collapsed
    // One-shot message shown in the context line (e.g. why a delete was
    // refused). Cleared by the next refresh.
    QString transient_;
    bool syncing_{false};
    bool hasCell_{false};
};

} // namespace ink
