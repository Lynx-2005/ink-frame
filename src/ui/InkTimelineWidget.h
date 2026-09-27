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
#include "core/InkParams.h"
#include "ui/InkIcons.h"
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>


namespace ink {
class CameraLayer;
class Document;
class PaintLayer;
class ParamBlock;

struct TimelineSel {
    QString trackId;
    int frame{-1}; // a cell is identified by its start frame
    bool operator==(const TimelineSel &o) const { return trackId == o.trackId && frame == o.frame; }
};
inline size_t qHash(const TimelineSel &s, size_t seed = 0) {
    return qHash(s.trackId, seed) ^ size_t(s.frame * 2654435761u);
}

// One keyframe in the lane: which layer's parameter, and on which frame.
// The keyframes live ON the paint layer; `adjId` holds the LAYER's stable id.
struct TimelineKey {
    QString adjId;
    int param{-1}; // (int)ParamId
    int frame{-1};
    bool operator==(const TimelineKey &o) const {
        return adjId == o.adjId && param == o.param && frame == o.frame;
    }
};
inline size_t qHash(const TimelineKey &k, size_t seed = 0) {
    return qHash(k.adjId, seed) ^ size_t(k.param * 2654435761u) ^ size_t(k.frame * 40503u);
}

class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(Document *doc, QWidget *parent = nullptr);
    void refresh();              // re-read model, repaint (keeps thumb cache)
    void invalidateCells();      // drop cached cel thumbnails (content changed)
    // Drop ONE cell's thumbnail (stroke-commit path): repainting the filmstrip
    // after every dab must not re-render every cell's composite synchronously.
    void invalidateCell(const QString &trackId, int key);
    double pxPerFrame() const { return pxPerFrame_; }
    void setPxPerFrame(double v, int anchorX = -1); // cursor-anchored zoom
    void fitToRange(); // fit full frame range into lane width
    // scrollbar interface (dock owns the bars)
    int contentWidth() const;
    int contentHeight() const;
    void setScroll(int hx, int vy);
    int hScroll() const { return hScroll_; }
    void ensureFrameVisible(int frame);
    // The keyed range of an animated layer, in widget coordinates (empty when
    // the layer has no keys). Paint, hover, drag and tests read the SAME rect.
    QRect keyedRangeRect(const QString &trackId) const;
    // cell selection
    QSet<TimelineSel> selection() const { return selection_; }
    void setSelection(const QSet<TimelineSel> &s);
    void clearSelection();
    bool hasSelection() const { return !selection_.isEmpty(); }
    // layer selection (drives grouping)
    QSet<QString> selectedLayers() const { return layerSel_; }
    void setSelectedLayers(const QSet<QString> &ids);
    QString selectedFolderId() const { return folderSel_; }
    void clearLayerSelection();
    // Track/header order as currently laid out ("folder:<id>" for headers,
    // else the track's stable id). Test seam: proves a folder camera paints
    // above its group and a layer camera directly below its target.
    QStringList trackRowOrder() const;
    // playback range (shading + ruler handles)
    void setRange(int in, int out);
    int rangeIn() const { return in_; }
    int rangeOut() const { return out_; }
    // selected keyframes (the bar and the easing graph share this selection)
    QSet<TimelineKey> keySelection() const { return keySel_; }
    void setKeySelection(const QSet<TimelineKey> &s) { keySel_ = s; update(); }
    void clearKeySelection() { keySel_.clear(); update(); }
    void noteAnimationChanged(); // drop caches that depend on key positions
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
    // Right-click a keyframe diamond: a menu for every parameter keyed at that
    // frame (interpolation, ease, delete).
    void keyFrameMenuRequested(const QPoint &globalPos, const QString &trackId, int frame);
    void renameRequested(const QString &trackId);
    void addCellRequested(const QString &trackId, int frame);
    // Blank cell inserted AT atFrame; every cell at/after it ripples one frame right.
    void insertCellRequested(const QString &trackId, int atFrame);
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
        // --- keyframes on the layer's own lane ---
        bool isAdjustment{false};  // always false now; kept for callers
        bool isCamera{false};      // camera row: no cells, lane shows diamonds
        int cameraScope{0};        // (int)CameraScope when isCamera
        // Folder-scope cameras are STORED as their folder's first child but
        // PAINTED on a row above the group header: they frame the whole group,
        // so showing them as sub-rows inside it reads as "camera for a layer".
        bool detachedCam{false};   // painted outside/above its folder block
        int param{-1};             // (int)ParamId on a parameter row
        bool animated{false};      // the layer holds transform keyframes
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
    // Camera lanes own no cells: a faint centre rule plus a ghost diamond
    // under the pointer (what a double-click would key) — never a +CELL box.
    void paintCameraLane(QPainter &p, const RowInfo &r);
    // The diamonds riding on an ANIMATED PAINT LAYER's lane (one per keyed
    // frame), drawn over the cells. Camera overload draws the same diamonds
    // over an empty lane (cameras own no cells).
    void paintKeyLane(QPainter &p, const RowInfo &r, const PaintLayer &layer);
    void paintKeyLane(QPainter &p, const RowInfo &r, const CameraLayer &cam);
    void paintKeyLaneParams(QPainter &p, const RowInfo &r, const ParamBlock &params);
    QPainterPath diamondAt(int cx, int cy, int radius, bool hold) const;

    // --- keyframe geometry on a paint row (shared by paint + hit-testing) ---
    QRect inlineEyeRect(const RowInfo &r) const;
    QRect inlineLockRect(const RowInfo &r) const;
    // Frame under x, or -1 when the pointer is not over the lane.
    int frameAtLaneX(int x) const;
    // 0 = nothing, 1 = a keyframe diamond. Keys sit on the layer's own lane.
    int keyHitTest(const RowInfo &r, const QPoint &pos, int *frameOut) const;
    PaintLayer *animatedLayerOf(const QString &trackId) const;
    // The span of an animated layer's keys, in widget coordinates: ONE source
    // of truth for painting, hover and drag of the retime grips.
    QRect keyedRangeRect(const RowInfo &r) const;
    // -1 = left grip (retime in), +1 = right grip (retime out), 2 = body (move
    // all keys), 0 = not on the keyed range at all.
    int keyedRangeHit(const RowInfo &r, int x) const;
    // Every keyed frame of a layer (summary lane) or of one parameter.
    std::vector<int> keyFrames(const PaintLayer &layer, int param) const;
    std::vector<int> keyFrames(const CameraLayer &cam, int param) const;
    std::vector<int> keyFramesOf(const QString &trackId, int param) const;
    CameraLayer *cameraOf(const QString &trackId) const;
    const ParamBlock *paramsOf(const QString &trackId) const;
    // Human target of a camera row ("Layer 1", "Folder 1", "whole shot").
    QString cameraTargetName(const CameraLayer *cam) const;
    void paintRuler(QPainter &p);
    void paintPlayhead(QPainter &p);
    void paintRubberBand(QPainter &p);
    // Thumbnail of the whole cell (its layer stack composited), cached per
    // track+key and cleared by invalidateCells().
    QPixmap thumbFor(const QString &trackId, int key);
    // Any pixel content in that cell's stack? (blank vs drawn)
    bool cellHasContent(const QString &trackId, int key) const;
    void setFrameFromX(int x);
    void updateHover(const QPoint &pos);
    void applyTooltip(const QPoint &pos);
    // Right-click context menu (deferred until release so a right-drag can
    // rubber-band instead). pos is in widget coordinates.
    void showContextMenu(const QPoint &pos);

    Document *doc_;
    double pxPerFrame_{10.0};
    int hScroll_{0};
    int vScroll_{0};
    int in_{0}, out_{0};
    QSet<TimelineSel> selection_;
    QSet<QString> layerSel_;
    QSet<TimelineKey> keySel_;
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
    int hoverKeyFrame_{-1}; // keyframe diamond under the pointer
    int hoverGutterPart_{0}; // 1 = stopwatch, 2 = keyframe diamond in the gutter
    // keyframe drag state: every selected key moves together, committed once
    bool draggingKeys_{false};
    QString keyDragAdj_;
    std::vector<std::pair<int, int>> keyDragBase_; // (param, current frame)
    int keyDragOrigin_{-1};
    bool keyMoved_{false};
    // drag state
    bool scrubbing_{false};
    bool movingCell_{false};
    bool resizingCell_{false};
    // --- keyed range drag on a paint row: trim (edges) or retime (body, keys
    //     travel). The pre-drag snapshot makes the whole gesture one undo
    //     step; the drag applies its delta on top of the SNAPSHOT each move,
    //     so it is idempotent and a no-move release pushes nothing.
    bool draggingBar_{false};
    int barDragSide_{0};     // -1 trim-in, 0 move, +1 trim-out
    QString barDragTrack_;
    int barDragOrigStart_{-1}, barDragOrigEnd_{-1};
    int barDragGrabFrame_{0}; // frame offset of the pointer inside the keys
    ParamBlock barDragBaseParams_;
    bool barMoved_{false};
    bool rubberBanding_{false};
    int dragRange_{0}; // 0 none, 1 in handle, 2 out handle
    int resizeSide_{0};      // -1 grow/shrink from the left, +1 from the right
    int resizeCellStart_{-1};
    QPoint rubberStart_;
    QRect rubberRect_;
    // Right-drag rubber band: armed on right-press, active once the pointer
    // moves past the drag threshold. Plain right-click (no drag) still opens
    // the context menu on release.
    bool rightArmed_{false};
    bool rightBandActive_{false};
    QPoint rightPressPos_;
    QString dragTrack_;
    int dragFromKey_{-1};
    int dragOrigFrame_{-1};
    QPoint pressPos_;
    QHash<QString, QPixmap> thumbs_;
};

} // namespace ink
