#pragma once
// Layer dock (Kosei, CSP-style): folders are MANDATORY — tracks always live
// inside a user-named folder, even for single-layer projects.
// Columns: [thumb+name] [eye] [lock] [opacity]. Inline rename edits the name
// column only (no glyph parsing). Eye/lock toggle by clicking their cells.
// New layers insert instantly ABOVE the current layer (no dialogs).
// Bottom inspector: opacity slider + blend combo for the current track.
#include <QDockWidget>

class QComboBox;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;

namespace ink {
class Document;
class GroupLayer;
class PaintLayer;

class LayerDock : public QDockWidget {
    Q_OBJECT
public:
    explicit LayerDock(Document *doc, QWidget *parent = nullptr);
    void refresh(); // selection/scroll preserving resync
signals:
    void layersChanged();
private slots:
    void onAddFolder();
    void onAddPaint();
    void onDuplicate();
    void onRemove();
    void onCellClicked(QTreeWidgetItem *item, int col);
    void onItemChanged(QTreeWidgetItem *cur, QTreeWidgetItem *prev);
    void onItemRenamed(QTreeWidgetItem *item, int col);
    void onDropMove();
    void onContextMenu(const QPoint &pos);
    void onOpacitySlider(int v);
    void onBlendChanged(int idx);
    void syncInspector();
private:
    QTreeWidgetItem *folderItem(GroupLayer *g);
    QTreeWidgetItem *trackItem(PaintLayer *l, QTreeWidgetItem *parent);
    void updateThumb(QTreeWidgetItem *it, PaintLayer *l);
    QString selectedStableId() const;
    void reselect(const QString &stableId);
    Document *doc_;
    QTreeWidget *tree_{nullptr};
    QSlider *opacitySlider_{nullptr};
    QComboBox *blendBox_{nullptr};
    bool syncing_{false};
};

} // namespace ink
