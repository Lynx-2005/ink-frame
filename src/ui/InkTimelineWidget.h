#pragma once
// Timeline grid: sticky gutter (layer list) + ruler + paper cells.
// Custom QWidget paint; scrolling via external QScrollBars owned by the dock
// so content can never be lost.
//
// Model: a track with no key is an EMPTY track (no cell drawn at all). Each
// key starts a CELL that holds until the next key, and the cells of one track
// are painted as ONE continuous exposure bar of paper sheets separated by a
// 1px divider — never as separate blocks with gaps.
//
// Layers are managed in the gutter: click selects, Ctrl/Shift multi-selects,
// and the header buttons create/group/ungroup folders.
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QWidget>
#include <vector>

#include "ui/InkIcons.h"

namespace ink {
class Document;

struct TimelineSel {
    QString trackId;
    int frame{-1}; // a cell is identified by its start frame
    bool operator==(const TimelineSel &o) const { return trackId == o.trackId && frame == o.frame; }
};
inline size_t qHash(const TimelineSel &s, size_t seed = 0) {
    return qHash(s.trackId, seed) ^ size_t(s.frame * 2654435761u);
}

class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(Document *doc, QWidget *parent = nullptr);
    void refresh();              // re-read model, repaint (keeps thumb cache)
    void invalidateCells();      // drop cached cel thumbnails (content changed)
    double pxPerFrame() const { return pxPerFrame_; }
    void setPxPerFrame(double v, int anchorX = -1); // cursor-anchored zoom
    void fitToRange(); // fit full frame range into lane width
    // scrollbar interface (dock owns the bars)
    int contentWidth() const;
    int contentHeight() const;
    void setScroll(int hx, int vy);
    int hScroll() const { return hScroll_; }
    void ensureFrameVisible(int frame);
    // cell selection
    QSet<TimelineSel> selection() const { return selection_; }
    void setSelection(const QSet<TimelineSel> &s);
    void clearSelection();
    bool hasSelection() const { return !selection_.isEmpty(); }
    // layer selection (drives grouping)
    QSet<QString> selectedLayers() const { return layerSel_; }
    void setSelectedLayers(const QSet<QString> &ids) { layerSel_ = ids; folderSel_.clear(); update(); }
    QString selectedFolderId() const { return folderSel_; }
    void clearLayerSelection() { layerSel_.clear(); folderSel_.clear(); update(); }
    // playback range (shading + ruler handles)
    void setRange(int in, int out);
    int rangeIn() const { return in_; }
    int rangeOut() const { return out_; }
signals:
    void frameChanged(int frame);
    void keyChanged();
    void selectionChanged();
    void scrollRequest(int hx, int vy); // widget asks dock to move bars
    void zoomChanged(double pxPerFrame);
    void trackMenuRequested(const QPoint &globalPos, const QString &trackId);
    void laneMenuRequested(const QPoint &globalPos, const QString &trackId, int frame);
    void rulerMenuRequested(const QPoint &globalPos);
    void layerSelectionChanged();
    void addLayerRequested();
    void addGroupRequested();
    void groupSelectionRequested();
    void ungroupRequested();
    void renameRequested(const QString &trackId);
    void addCellRequested(const QString &trackId, int frame);
    void rangeChangeRequested(int in, int out);
protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    struct RowInfo {
        bool isFolderHeader{false};
        bool expanded{true};
        int childCount{0};
        QString folderId;
        QString trackId;
        QString label;
        int y{0}, h{0};
    };
    // Single source of truth for gutter column x positions.
    struct Columns {
        int spine, expander, check, thumb, name, eye, lock, opacityRight;
    };
    enum class HeadBtn { AddLayer, AddGroup, GroupSelected, Ungroup };
    struct HeaderBtn {
        HeadBtn id;
        QRect rect;
        IconId icon;
        QString tip;
        bool enabled{true};
    };

    std::vector<RowInfo> layoutRows() const;
    std::vector<HeaderBtn> headerButtons() const;
    Columns columns() const;
    int totalRowsHeight() const;
    int frameAtX(int x) const;
    int xAtFrame(int f) const;
    int rowIndexAt(const std::vector<RowInfo> &rows, int y) const;
    // Layer-selection checkbox: rect + 'x is on the box' test (folder rows use
    // the same slot to select every layer inside the folder).
    QRect checkBoxRect(const RowInfo &r) const;
    static bool inCheckBox(const QRect &box, int x);
    void toggleLayerSelection(const QString &trackId);
    void setFolderSelection(const QString &folderId, bool on);
    // Range handles: ONE source of truth for both painting and hit-testing, so
    // what is drawn is exactly what can be grabbed (side 1 = in, 2 = out).
    QRect rangeGrabRect(int side) const;
    int rangeHitAt(int x) const;

    void paintHeaderRows(QPainter &p);
    void paintFolderRow(QPainter &p, const RowInfo &r);
    void paintTrackRow(QPainter &p, const RowInfo &r, bool isCurrent);
    void paintCells(QPainter &p, const RowInfo &r, int hueIdx);
    void paintEmptyLane(QPainter &p, const RowInfo &r);
    void paintRuler(QPainter &p);
    void paintPlayhead(QPainter &p);
    void paintRubberBand(QPainter &p);
    QPixmap thumbFor(const QString &trackId, int key, const void *device);
    void setFrameFromX(int x);
    void updateHover(const QPoint &pos);
    void applyTooltip(const QPoint &pos);

    Document *doc_;
    double pxPerFrame_{10.0};
    int hScroll_{0};
    int vScroll_{0};
    int in_{0}, out_{0};
    QSet<TimelineSel> selection_;
    QSet<QString> layerSel_;
    QString anchorId_;
    QString folderSel_;
    // hover
    QString hoverTrack_;
    int hoverFrame_{-1};
    int hoverBtn_{-1};
    int hoverEdge_{0};      // -1 = cell's left edge under the pointer, +1 right
    int hoverRange_{0};     // 1 = in handle under the pointer, 2 = out, 0 = none
    bool hoverCheck_{false}; // pointer is on a gutter selection checkbox
    QString hoverCellKey_;  // "<track>:<start>" of the hovered cell
    // drag state
    bool scrubbing_{false};
    bool movingCell_{false};
    bool resizingCell_{false};
    bool rubberBanding_{false};
    int dragRange_{0}; // 0 none, 1 in handle, 2 out handle
    int resizeSide_{0};      // -1 grow/shrink from the left, +1 from the right
    int resizeCellStart_{-1};
    QPoint rubberStart_;
    QRect rubberRect_;
    QString dragTrack_;
    int dragFromKey_{-1};
    int dragOrigFrame_{-1};
    QPoint pressPos_;
    QHash<QString, QPixmap> thumbs_;
};

} // namespace ink
